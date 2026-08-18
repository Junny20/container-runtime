A small `runc`-style container runtime. It turns an
OCI bundle on disk (a root filesystem plus a `config.json`) into an isolated
process, and manages that process through the OCI lifecycle verbs
`create` / `start` / `state` / `delete`.
