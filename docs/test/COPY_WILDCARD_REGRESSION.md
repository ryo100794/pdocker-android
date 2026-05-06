# Dockerfile COPY Wildcard Regression

Date: 2026-05-06

This regression covers a backend builder/path-resolution change that broke an
existing Docker-compatible Dockerfile construct:

```dockerfile
COPY scripts/pdocker-* /usr/local/bin/
```

The failure was not a Dockerfile/template change. The bundled default workspace
already used this construct, and the backend builder stopped expanding the
source pattern.

The practical trigger can be a cache/state change: if an existing image or
inline cache is reused, this broken COPY path is not exercised. Any builder,
cache, path, or image-state refactor must therefore include a clean build route
that cannot be satisfied by an old materialized image.

## Red Evidence

The previous builder logic was checked by running the same minimal build case
against `git show HEAD:docker-proot-setup/bin/pdockerd` before applying the
fix. It reproduced the failure:

```text
COPY failed: scripts/pdocker-* not found
```

It also continued the build after the failed copy and produced an empty COPY
layer, which meant later steps could fail away from the real cause.

## Green Evidence

The fixed builder was checked on the Android device through the Engine `/build`
API using the same public Dockerfile construct. The build expanded both matches:

```text
scripts/pdocker-a -> /usr/local/bin/
scripts/pdocker-b -> /usr/local/bin/
Successfully tagged local/copy-wildcard-test:latest
```

## Guard

`tests.test_dockerfile_copy_compat` now exercises the actual
`execute_dockerfile_build` path, not only the helper function. It covers:

- positive COPY wildcard expansion;
- missing wildcard source rejection;
- context escape rejection;
- the bundled default workspace COPY pattern.

Future parser, builder, filesystem mediation, or path rewrite changes must keep
this public-route regression green before they can be treated as compatible.
