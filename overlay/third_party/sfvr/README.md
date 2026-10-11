# Vendored sfvr snapshot

The canonical helper library is [astromaddie/sfvr](https://github.com/astromaddie/sfvr), licensed under MIT; the matching notice is copied to `LICENSE`.

This Vice City snapshot is based on sfvr `ad2f545`, with `sfvr_adjust` and `sfvr_camera` added from `b09b6e1cb68ac17cf5e68bdaa867fefcd2e0121c`. The project keeps its `refresh_rate` setting default at 120 rather than the upstream library's 90. Other differences are comment-only. This is therefore a port-specific snapshot, not a byte-identical copy of the current sfvr repository.

The upstream API and build notes are in [sfvr's README](https://github.com/astromaddie/sfvr/blob/b09b6e1cb68ac17cf5e68bdaa867fefcd2e0121c/README.md) and [standard](https://github.com/astromaddie/sfvr/blob/b09b6e1cb68ac17cf5e68bdaa867fefcd2e0121c/STANDARD.md). Do not overwrite this directory wholesale without reviewing the port-specific setting and build integration.
