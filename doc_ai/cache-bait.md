# Cache Bait Design

`cacheBait` is a preparation strategy for P2 external class-file copying. It is not a replacement for the normal JVMTI dump path. Its job is to make selected HotSpot builds keep a readable cached class-file payload that the external reader can copy later.

## Shape

The agent still registers the normal `ClassFileLoadHook` and still applies the shared dump filter before doing any work. When `cacheBait=true` and a class name matches, the hook allocates a JVMTI buffer, copies the incoming class bytes unchanged, and returns that buffer through `new_class_data`.

HotSpot treats the returned payload as a transformed class-file payload even though the bytes are identical. On validated Corretto 8u352 targets, this populates the hidden cached-class-file state that P2 reads through the inferred `InstanceKlass` slot.

## CLI Flow

`jdumper dump --external --cache-bait -o <out> [filters]` runs two sessions:

1. An agent-backed preparation session under `<out>/_bait`.
2. The normal external memory-copy session under `<out>`.

The preparation session uses the same filters as the external copy. Keeping its metadata under `_bait` lets failures be diagnosed without replacing the final external `_meta.json`. The final external metadata also carries a compact `cacheBait*` summary, including the child session directory, terminal state,class writes, and matched sweep count.

## Boundaries

- The bait hook only runs for filter-matched class names.
- The class bytes are copied byte-for-byte; no bytecode rewrite is performed.
- The strategy is validated on Corretto 8u352. Other JDK families still require their source gate and cache behavior to be checked separately.
- If the target JVM does not expose or infer a readable class-byte source,`--external` still fails after the preparation step with the normal source gate diagnostics.
