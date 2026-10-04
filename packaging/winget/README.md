# winget manifest

`0.1.0/` is the manifest set for `winget install ProvidenceCrafts.redstone`, in the layout
[winget-pkgs](https://github.com/microsoft/winget-pkgs) expects under
`manifests/p/ProvidenceCrafts/redstone/<version>/`.

The installer hash is a placeholder until the release exists. For each release:

1. Push the tag `vX.Y.Z`; `.github/workflows/release.yml` publishes
   `redstone-X.Y.Z-windows-x86_64.zip` and `SHA256SUMS`.
2. Copy `0.1.0/` to `X.Y.Z/` and replace the version, the URL and `InstallerSha256` with the line
   for the zip from `SHA256SUMS`.
3. Check locally on Windows: `winget validate --manifest X.Y.Z` and
   `winget install --manifest X.Y.Z`.
4. Open a pull request adding the directory to `microsoft/winget-pkgs`
   (`wingetcreate submit X.Y.Z` does steps 2 and 4 in one go).

`License` assumes GPL-3.0-or-later; change it to `GPL-3.0-only` if the LICENSE header says so.
