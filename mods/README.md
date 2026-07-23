# Mods

A mod is a directory with a `manifest.json`
describing capabilities that `zuxhook`'s on-device runtime applies at boot; assets
are built and the directory is deployed with `modkit/mod-apply.py`.

Authoring guide and the capability reference: <https://wiki.zune.moe>.

Platform compatibility is automatic: a mod declares what it does (its actions), and the mod
manager installs it only where Lyra provides every capability it uses. You write no version or
requirement metadata. The capabilities a mod can use are listed in
[`CAPABILITIES.md`](../CAPABILITIES.md).
