#pragma once
#include "llvm/Support/Error.h"
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

template <typename T, typename G>
concept MoveAndInvocable =
  std::is_object_v<T> && std::move_constructible<T> && std::invocable<T, G>;

template <typename Test, template <typename...> class Ref>
struct is_specialization : std::false_type {};

template <template <typename...> class Ref, typename... Args>
struct is_specialization<Ref<Args...>, Ref> : std::true_type {};

template <typename Src, typename Dst> struct environment_cast {
  Dst cast(Src &src);
};

template <typename Env, typename V> class ReaderIO {

  class BaseFunction {
  public:
    virtual llvm::Expected<V> invoke(Env env) = 0;
    virtual ~BaseFunction() {};
  };

  template <MoveAndInvocable<Env> Fn>
  class ErasureFunction : public BaseFunction {
  public:
    llvm::Expected<V> invoke(Env env) override { return std::invoke(fn, env); };
    ErasureFunction(Fn &&ff) : fn(std::forward<Fn>(ff)) {}
    ~ErasureFunction() = default;
    ErasureFunction(const ErasureFunction &copy) = delete;
    ErasureFunction &operator=(const ErasureFunction &copy_assign) = delete;

  private:
    Fn fn;
  };

public:
  template <std::invocable<Env> T> ReaderIO(T &&ff) {
    ops.push_back(std::make_unique<ErasureFunction<T>>(std::forward<T>(ff)));
  }
  ~ReaderIO() {
    destroy();
  }

  void destroy() {
    while (!ops.empty()) {
      ops.pop_back();
    }
  }

  template <typename Env2> ReaderIO(ReaderIO<Env2, V> &&castable) {
    auto f = [castable = std::move(castable)](Env e) mutable {
      struct environment_cast<Env, Env2> cast;
      return castable.build(cast.cast(e));
    };
    ops.push_back(std::make_unique<ErasureFunction<decltype(f)>>(std::move(f)));
  }

  ReaderIO(ReaderIO &&other) = default;
  ReaderIO &operator=(ReaderIO &&move_assign) = default;
  ReaderIO(const ReaderIO &copy) = delete;
  ReaderIO &operator=(const ReaderIO &copy_assign) = delete;

  llvm::Expected<V> build(Env ir) {
    assert(!ops.empty() && "value has been consumed or moved.");
    unsigned i = 0;
    while(i < ops.size()) {
      llvm::Expected<V> ret = ops[i]->invoke(ir);
      if (auto err = ret.takeError()) {
        destroy();
        return err;
      }
      i++;
      if (i == ops.size()) {
        destroy();
        return ret;
      }
    }
    __builtin_unreachable();
  };

  struct promise_type {
    Env *to_be_filled = nullptr;
    std::optional<llvm::Expected<V>> return_value_;
    ReaderIO<Env, V> get_return_object() {
      using Handle = std::coroutine_handle<promise_type>;
      auto destroy_frame = [](promise_type *p) { Handle::from_promise(*p).destroy(); };
      std::unique_ptr<promise_type, decltype(destroy_frame)> frame(this);
      return ReaderIO<Env, V>([frame = std::move(frame)](Env ctx) mutable -> llvm::Expected<V> {
        auto owner = std::move(frame);
        auto &promise = *owner;
        promise.to_be_filled = &ctx;
        auto h = Handle::from_promise(promise);
        h.resume();
        assert(promise.return_value_ && "no returnvalue");
        auto r = std::move(*promise.return_value_);
        assert(
          (h.done() == bool(r)) &&
          "unexpected suspension of coroutine"
        );
        return r;
      });
    };

    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() {}

    template <std::move_constructible ResumeVal> struct trivial_value_awaiter {
      llvm::Expected<ResumeVal> val;
      bool await_ready() noexcept { return bool(val); }
      void await_suspend(const std::coroutine_handle<promise_type> &h) {
        h.promise().return_value_.emplace(val.takeError());
      }
      ResumeVal await_resume() {
        return std::forward<ResumeVal>(val.get());
      }
    };
    template <std::move_constructible S>
    trivial_value_awaiter<S> yield_value(ReaderIO<Env, S> &&s) {
      assert(to_be_filled);
      return {s.build(*to_be_filled)};
    };

    template <typename Env2, std::move_constructible S>
    trivial_value_awaiter<S> yield_value(ReaderIO<Env2, S> &&s) {
      assert(to_be_filled);
      struct environment_cast<Env, Env2> cast;
      return {s.build(cast.cast(*to_be_filled))};
    };

    void return_value(V value) {
      return_value_.emplace(std::forward<V>(value));
    };
  };

  ReaderIO &concat(ReaderIO&& b) {
    for(auto &f : b.ops) {
      ops.push_back(std::move(f));
    }
    b.ops.clear();
    return *this;
  };

private:
  std::vector<std::unique_ptr<BaseFunction>> ops;
};

/* bind */
template <typename Env, typename V, std::invocable<V> Func>
auto operator>>=(ReaderIO<Env, V> &&src, Func &&fn) {
  using ret_value_type =
    typename decltype(fn(std::declval<V>()).build(std::declval<Env>())
    )::value_type;
  using R = ReaderIO<Env, ret_value_type>;
  return R(
    [src0 = std::move(src), fn = std::forward<Func>(fn)](auto context
    ) mutable -> llvm::Expected<ret_value_type> {
      auto ret = src0.build(context);
      if (auto err = ret.takeError()) {
        return std::move(err);
      }
      return fn(ret.get()).build(context);
    }
  );
}

/* map */
template <typename Env, typename V, std::invocable<V> Func>
auto operator|(ReaderIO<Env, V> &&src, Func &&fn) {
  using ret_value_type = decltype(fn(std::declval<V>()));
  using R = ReaderIO<Env, ret_value_type>;
  return R(
    [src = std::move(src), fn = std::forward<Func>(fn)](auto context
    ) mutable -> llvm::Expected<ret_value_type> {
      auto ret = src.build(context);
      if (auto err = ret.takeError()) {
        return std::move(err);
      }
      return fn(ret.get());
    }
  );
}

/* in-place sequence: a = a then b */
template <typename Env, typename A>
ReaderIO<Env, A> &operator<<(ReaderIO<Env, A> &a, ReaderIO<Env, A> &&b) {
  return a.concat(std::move(b));
};

/* in-place bind: a = fn(result of a) */
template <typename Env, typename V, std::invocable<V> Func>
ReaderIO<Env, V> &operator>>(ReaderIO<Env, V> &a, Func &&fn) {
  a = std::move(a) >>= std::forward<Func>(fn);
  return a;
};

template <typename Env, typename A, typename B, std::move_constructible Func>
auto lift(ReaderIO<Env, A> &&a, ReaderIO<Env, B> &&b, Func &&func) {
  return std::move(a) >>= [func = std::forward<Func>(func),
                           b = std::move(b)](auto a) mutable {
    return std::move(b) >>=
           [a, func = std::forward<Func>(func)](auto b) { return func(a, b); };
  };
};

template <typename Env, typename A, typename B, typename C, typename Func>
auto lift(
  ReaderIO<Env, A> &&a, ReaderIO<Env, B> &&b, ReaderIO<Env, C> &&c, Func &&func
) {
  return std::move(a) >>= [func = std::forward<Func>(func), b = std::move(b),
                           c = std::move(c)](auto a) mutable {
    return std::move(b) >>= [a, func = std::forward<Func>(func),
                             c = std::move(c)](auto b) mutable {
      return std::move(c) >>= [a, b, func = std::forward<Func>(func)](auto c) {
        return func(a, b, c);
      };
    };
  };
};

// template <typename Env, typename A, typename B, typename C, typename
// D,typename Func> auto lift(const ReaderIO<Env,A>& a, const ReaderIO<Env,B>&
// b, const ReaderIO<Env,C>& c,  const ReaderIO<Env,D>& d, Func&& func) {
//   return a >>= [=](auto a) {
//     return b >>= [=](auto b) {
//       return c >>= [=](auto c) {
//         return d >>= [=](auto d) {
//           return func(a,b,c,d);
//         };
//       };
//     };
//   };
// };
