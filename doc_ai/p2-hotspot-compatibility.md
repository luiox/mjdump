# P2 HotSpot Compatibility Matrix

本文记录 `jdumper dump --external` 在不同 HotSpot 家族上的当前能力边界。P2 的原则是先做显式 gate：能外部读取就写出 class bytes，不能读取就输出稳定`errorCode` 和原因，而不是假装成功。

## Compatibility

| JVM | Walk source | Byte source | Current result | Notes |
|-----|-------------|-------------|----------------|-------|
| Corretto 8u352 | `systemDictionary` | `hotspot8_hidden_cached_class_file` | ✅ `--external --cache-bait` writes matched class bytes | HotSpot 8 隐藏 `_cached_class_file`，当前通过相邻字段保守推断 hidden slot；无 bait 时通常得到 `no_cached_class_bytes` |
| Corretto 17.0.5 | `cldg` | none | ✅ explicit negative gate | CLDG 遍历、Klass 名称和过滤命中可用；`InstanceKlass::_cached_class_file` 缺失，`status` 输出 `errorCode=source_unavailable` |
| Corretto 21.0.4 | `cldg` | none | ✅ explicit negative gate | 与 JDK 17 类似，外部身份识别可用，但 class-byte source gate 不可用 |

## Status Fields

External sessions always write `<out>/_meta.json`. The important compatibility fields are:

- `p2WalkSource`: `cldg` or `systemDictionary`.
- `p2Source`: selected class-byte source, or `none`.
- `p2SourceAvailable`: whether byte copying is allowed.
- `p2SourceReason`: human-readable unavailable reason.
- `errorCode`: stable failure bucket such as `source_unavailable` or `no_cached_class_bytes`.
- `cacheBaitState`: terminal state of the optional `_bait` child session.

## Validation Log

Validated on 2026-07-06:

- Corretto 8u352 `--external --cache-bait --class Sleep8`
  - `p2WalkSource=systemDictionary`
  - `p2Source=hotspot8_hidden_cached_class_file`
  - `p2ByteWrites=1`
  - dumped `Sleep8.class` SHA-256 equals the javac output
- Corretto 8u352 `--external --class Sleep8NoBait`
  - `status=failed`
  - `errorCode=no_cached_class_bytes`
  - `p2ByteNullCached=1`
- Corretto 17.0.5 `--external --class SleepCompat17`
  - `p2WalkSource=cldg`
  - `p2SourceAvailable=false`
  - `p2SourceReason=InstanceKlass::_cached_class_file missing`
  - `errorCode=source_unavailable`
- Corretto 21.0.4 `--external --class SleepCompat21`
  - `p2WalkSource=cldg`
  - `p2SourceAvailable=false`
  - `p2SourceReason=InstanceKlass::_cached_class_file missing`
  - `errorCode=source_unavailable`

## Next Compatibility Work

JDK 17+ still needs a non-`_cached_class_file` byte source before P2 can dump bytes without an in-process agent. Candidate directions:

- locate a version-specific class-file mirror or archived parser payload;
- add a controlled in-process preparation step that exposes a readable source;
- keep P2 identity-only on those versions and route byte dump through P1 until a stable byte source is proven.
