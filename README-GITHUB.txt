GitHub release archives do not build
====================================

This applies to the "Download ZIP" / "Source code" archives that GitHub
generates for https://github.com/JXMP-Audio/Jxmpose.

JXMPOSE (like Ardour, which it is based on) derives its program version from
git tags via `git describe`, so a full git clone with tags is needed to
build. GitHub cannot add a version file to its generated archives.

Clone the repository instead:

    git clone https://github.com/JXMP-Audio/Jxmpose.git

If `git describe` finds no tags, fetch the upstream Ardour release tags:

    git fetch --no-tags https://github.com/Ardour/ardour.git 'refs/tags/9.*:refs/tags/9.*'

See README.md for build instructions.
