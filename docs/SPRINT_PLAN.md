# Sprint Plan

The project is developed incrementally. Member roles rotate so that all three members contribute code, tests, design, and review work.

| Sprint | Goal | Member A | Member B | Member C | Milestone |
| --- | --- | --- | --- | --- | --- |
| 0 | Project setup | Requirements and backlog | Repository/build plan | Test strategy and workflow | Planning complete |
| 1 | Application skeleton | CLI entry | CMake/build scripts | Smoke test and README | Program builds |
| 2 | Basic backup | Directory scanner | File copy and path safety | Backup integration tests | Backup works |
| 3 | Manifest and restore | Manifest model | Restore and metadata | Verification tests | Basic version |
| 4 | Custom filters | Type/name filters | Path/size filters | Time filters and tests | Filtering version |
| 5 | Archive | SBA format | Pack command | Unpack/security tests | Archive version |
| 6 | Compression/encryption | RLE | OpenSSL AES-256-GCM | Password and authentication tests | Secure archive |
| 7 | Scheduled backup | Snapshot scheduler | Retention policy | Scheduler tests | Automation version |
| 8 | GUI | Native Win32 shell | GUI command integration | GUI build/test/docs | GUI version |
| 9 | Quality | Error handling | Integration tests | Build/demo scripts | Release candidate |
| 10 | Release | Design document | Test report evidence | Demo and release notes | v1.0.0 |

## Definition of done

A sprint task is complete only when:

- implementation is committed by the assigned member;
- a Pull Request targets `develop`;
- another member reviews it;
- required tests pass;
- the corresponding planning task contains the PR link and result;
- user-facing behavior is documented when applicable.

## GitHub release route

```text
feature/* -> develop -> release/v1.0 -> main -> tag v1.0.0
```
