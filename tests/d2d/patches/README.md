# d2d patch series

This project's changes to d2d (`third_party/d2d`, psi29a/d2decomped), applied by `tests/cpu/prepare.sh` as one commit per `*.patch` on top of the pinned commit, in `build/d2d/d2d`. The series is empty for now.

- The patches change d2d's files, so they are GPL-3.0-or-later like those files.
- To change d2d, edit and commit in `build/d2d/d2d` from inside the container (`scripts/docker.sh shell hosttest`), then run `make -C tests/d2d export` there. Then commit the `.patch` files here.
- A new pin is a deliberate change: move the submodule, then update `third_party/deps.lock` and `THIRD_PARTY_NOTICES.md`. The series must still apply.
