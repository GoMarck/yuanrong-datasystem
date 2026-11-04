#!/usr/bin/env bash
# Keep source origin and immutable revision shared by download and build checks.
case "${MOONCAKE_SOURCE_PROFILE:-legacy}" in
    legacy)
        mooncake_source_repo=https://github.com/LinQuickDev/Mooncake.git
        mooncake_source_revision=89520212377af7114bbba4177021ba092d78ed11
        mooncake_source_tag=legacy-8952021
        ;;
    official-v0.3.13.post1)
        mooncake_source_repo=https://github.com/kvcache-ai/Mooncake.git
        mooncake_source_revision=719735896c86b56fabec6cf3e825fb2ea640597a
        mooncake_source_tag=v0.3.13.post1
        ;;
    *)
        echo 'Unknown MOONCAKE_SOURCE_PROFILE; use legacy or official-v0.3.13.post1' >&2
        exit 2
        ;;
esac
