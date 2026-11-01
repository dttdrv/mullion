# Signing Mullion's NGX core

NVIDIA's NGX library, which DLSS games link, loads its core (`_nvngx.dll` or `nvngx.dll`, from the folder its registry
values name) only if the file's Authenticode signature chains to a trusted root. Unsigned, it fails with
`CERT_E_UNTRUSTEDROOT` (0x800B0109) and the game reports DLSS as unsupported. NGX logs this when
`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` has `LogLevel` (DWORD 2), `EnableLogPathOverride` (DWORD 1) and
`LogPath` (a folder).

So Mullion's `nvngx.dll` is signed with Mullion's own key, and DXGI trusts its certificate:

- `mullion-ngx.pem` is the certificate: self-issued, "Mullion NGX", code signing only. It names Mullion,
  not NVIDIA; NGX asks only for a trusted chain.
- `src/dxgi/dxgi_ngx_certificate.h` is the same certificate in DER. With `MULLION_ENABLE_NVEXT=1`, DXGI adds it to the
  prefix's `Root` store, and sets `NGXCore\FullPath` and `nvlddmkm\NGXCore\NGXPath` to its own folder (System32 for a
  Wine builtin). The trust is the prefix's only.
- The private key is the repository secret `MULLION_NGX_SIGNING_KEY` (PEM). The CI `package` job signs both
  architectures' `nvngx.dll` with it; a run without it (a fork's) leaves them unsigned, with a warning.

To sign a local build: `osslsigncode sign -certs src/nvngx/signing/mullion-ngx.pem -key <key.pem> -n "Mullion NGX"
-h sha256 -in nvngx.dll -out nvngx-signed.dll`.

A new key needs a new certificate here and in `dxgi_ngx_certificate.h`
(`openssl x509 -in mullion-ngx.pem -outform der | xxd -i`), and the new secret.

## Using DLSS

A game sees DLSS when Mullion shows it an NVIDIA RTX GPU and NGX finds the signed core:

- `MULLION_ENABLE_NVEXT=1` in the game's environment;
- `dxgi.customVendorId = 10de`, `dxgi.customDeviceId = 2704` and `dxgi.customDeviceDesc = "NVIDIA GeForce RTX 4080"`
  (quoted: an unquoted value ends at its first space) in the game's section of the configuration;
- the signed `nvngx.dll` in the prefix's System32 and in Wine's own `lib/wine/<arch>-windows` (`install.sh` puts both).

Games using Streamline also load `nvapi64.dll` from System32. Steam's own processes hang when it is there, so those
need `nvapi64` disabled (`HKCU\Software\Wine\AppDefaults\<exe>\DllOverrides`, an empty value).
