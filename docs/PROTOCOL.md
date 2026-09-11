# PControl IPC version 1

Pipe name: `\\.\pipe\PControl.v1.<current-user-SID>.<session-ID>`.

A connection carries one request and one response. Each is a four-byte little-endian unsigned payload length followed by UTF-8 JSON, at most 4 MiB and 32 nested levels. After reading a response, the client writes byte `0x01` to acknowledge consumption before the server disconnects. This prevents dropping buffered response bytes during disconnect. Read/write transfers have a 3-second deadline and cancel outstanding overlapped operations before releasing buffers/handles. The single server thread handles one bounded connection at a time; a stalled client cannot block process monitoring. GUI IPC executes outside the window thread.

Request example:

```json
{"version":1,"command":"SetPriority","pid":1234,"creationTime":"134000000000000000","value":16384}
```

Response envelope:

```json
{"version":1,"success":false,"errorCode":5,"errorMessage":"OpenProcess PID 1234: Access is denied.","data":null}
```

`errorCode` is a Win32 code where applicable. Invalid JSON/types, unsupported commands, version mismatch, zero affinity, invalid CPU indices and stale creation times return explicit failure. Transport failures disconnect and become GUI status errors. Requests have no automatic retry for mutations: after a lost reply, use GetConfiguration/GetProcessDetails to establish the current state.

| Command | Fields beyond version/command | Data on success |
| --- | --- | --- |
| Ping / GetStatus | none | state, Core PID, configHealthy, intervalMs, processorGroups |
| GetProcessList | none | latest snapshot array |
| GetProcessDetails | pid, creationTime string | current detail object |
| SetPriority | pid, creationTime, value | reread details |
| SetAffinity | pid, creationTime, value `{group,cpus}` | reread details |
| GetConfiguration | none | current validated configuration |
| AddPersistentPriorityRule | name, value | updated configuration |
| RemovePersistentPriorityRule | name | updated configuration |
| AddPersistentAffinityRule | name, value `{group,cpus}` | updated configuration |
| RemovePersistentAffinityRule | name | updated configuration |
| SetUpdateInterval | value integer 250..60000 | updated configuration |
| ReloadConfiguration | none | empty object |
| Shutdown | none | empty object; graceful Core exit |

Priority values: Idle=64, Below Normal=16384, Normal=32, Above Normal=32768, High=128, Real Time=256. The GUI warns before submitting Real Time. Affinity masks and creation times are decimal strings to avoid loss of 64-bit precision in future clients.

The server DACL grants access only to the current user SID, rejects remote clients, verifies the connecting session, and uses first-instance creation. The Core mutex is in the local session namespace and includes the current user SID. The GUI sets identification-only security QoS when opening a pipe. There is no unrestricted Everyone/Users ACE, no automatic elevation, and no claim of executable authentication: other applications of the same user/session can send commands, including Shutdown. Do not expose this protocol as a system service without designing a privilege boundary. Multiple simultaneous clients are serialized and may need to retry when the pipe is busy.

Configuration and wire schema are intentionally separate versions. New command names and rule match types can be introduced without moving enforcement into the GUI. Stage 1 rejects unknown commands and unsupported match types.
