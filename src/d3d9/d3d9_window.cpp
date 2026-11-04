/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d9_window.hpp"
#include "d3d9_device.hpp"
#include <mutex>
#include <vector>

namespace mullion {

namespace {

struct HookedWindow {
  HWND window;
  WNDPROC proc;
  bool unicode;
  // null once the device let go while the app's own subclass sat on top of ours
  MTLD3D9Device *device;
};

std::mutex hooks_mutex;
std::vector<HookedWindow> hooks;

LRESULT CALLBACK
FocusWindowProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam) {
  HookedWindow hook = {};
  {
    std::lock_guard lock(hooks_mutex);
    for (auto &h : hooks)
      if (h.window == Window)
        hook = h;
  }
  if (!hook.proc)
    return IsWindowUnicode(Window) ? DefWindowProcW(Window, Message, wParam, lParam)
                                   : DefWindowProcA(Window, Message, wParam, lParam);

  if (hook.device && hook.device->FiltersMessages())
    return hook.unicode ? DefWindowProcW(Window, Message, wParam, lParam)
                        : DefWindowProcA(Window, Message, wParam, lParam);
  if (Message == WM_DESTROY && hook.device)
    D3D9UnhookFocusWindow(Window, hook.device);
  else if (hook.device)
    hook.device->ProcessFocusMessage(Window, Message, wParam, lParam, hook.unicode);

  return hook.unicode ? CallWindowProcW(hook.proc, Window, Message, wParam, lParam)
                      : CallWindowProcA(hook.proc, Window, Message, wParam, lParam);
}

} // namespace

bool
D3D9HookFocusWindow(HWND Window, MTLD3D9Device *pDevice) {
  std::lock_guard lock(hooks_mutex);
  for (auto &h : hooks) {
    if (h.window != Window)
      continue;
    if (h.device && h.device != pDevice)
      return false;
    h.device = pDevice;
    return true;
  }
  bool unicode = IsWindowUnicode(Window);
  WNDPROC proc = (WNDPROC)(unicode ? SetWindowLongPtrW(Window, GWLP_WNDPROC, (LONG_PTR)FocusWindowProc)
                                   : SetWindowLongPtrA(Window, GWLP_WNDPROC, (LONG_PTR)FocusWindowProc));
  if (!proc)
    return false;
  hooks.push_back({Window, proc, unicode, pDevice});
  return true;
}

void
D3D9UnhookFocusWindow(HWND Window, MTLD3D9Device *pDevice) {
  std::lock_guard lock(hooks_mutex);
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    if (it->window != Window || it->device != pDevice)
      continue;
    LONG_PTR current = it->unicode ? GetWindowLongPtrW(Window, GWLP_WNDPROC) : GetWindowLongPtrA(Window, GWLP_WNDPROC);
    if (current != (LONG_PTR)FocusWindowProc) {
      // the app subclassed the window after us; keep forwarding to the original procedure
      it->device = nullptr;
      return;
    }
    if (it->unicode)
      SetWindowLongPtrW(Window, GWLP_WNDPROC, (LONG_PTR)it->proc);
    else
      SetWindowLongPtrA(Window, GWLP_WNDPROC, (LONG_PTR)it->proc);
    hooks.erase(it);
    return;
  }
}

} // namespace mullion
