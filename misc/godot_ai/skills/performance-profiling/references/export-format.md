# The profiler capture export, record by record

The export is JSON Lines: one JSON object per line, discriminated by `type`. Every
`*_ms` field is milliseconds; every memory field is bytes. `t_ms` on every record is
editor-receipt time since the capture started — it is the one time axis shared by all
streams (the streams' own counters tick at different rates and are kept under
stream-specific names).

| type | one per | what it holds |
|---|---|---|
| `header` | file | capture options, units statement, `builtin_monitors` (names for `mon.raw` indices), column layouts |
| `sig` | unique function | `id` → `name` for script functions; `frame.funcs` rows reference these ids. Ids are file-local and stable even when the game's own profiler restarts mid-capture |
| `frame` | game process frame | `n` (the game's process frame number), `frame_ms`, `process_ms`, `physics_ms`, `physics_frame_ms`, `script_ms`, `servers` (category → item → ms), `funcs` rows `[sig_id, calls, self_ms, total_ms, internal_ms]` — each frame carries only that frame's top `max_functions` rows |
| `gpu` | completed rendered frame | `rs_frame`, `total_gpu_ms`, `total_cpu_ms`, `areas` rows `[name, cpu_ms, gpu_ms]` as per-pass deltas (the engine's `<`/`>` nesting markers are already stripped). GPU timestamps read back late, so `rs_frame` lags `frame.n`; repeated readbacks are deduplicated. Absent entirely under headless or software rendering |
| `mon` | ~1 second | converted headline fields (`fps`, `process_ms`, `physics_ms`, `static_mem`, `static_mem_max`, `video_mem`, `texture_mem`, `buffer_mem`, `objects`, `resources`, `nodes`, `orphan_nodes`, `draw_calls`) plus `raw`: every monitor untouched, in native units — time monitors in `raw` are **seconds** |
| `mon_names` | change | names of custom monitors appended to `raw` after the built-in list |
| `mem` | window edge | a precise CPU-memory sample requested from the game at start and at stop, because the 1 Hz monitor stream quantizes the edges |
| `vram` | snapshot | per-resource video memory, rows `[path, type, format, bytes]` sorted largest-first, `phase` start/end, `total_bytes` |
| `event` | anomaly | anything unusual mid-capture — most importantly the game's profiler being switched off underneath the capture (the editor's Profiler panel does this) and the re-enable that answered it |
| `total` | file | per-function times accumulated across the whole window, rows `[name, calls, self_ms, total_ms, internal_ms]`. **Prefer this to folding frames**: per-frame rows are each frame's top slice, and folding them under-counts steadily warm code |
| `summary` | file | the same aggregates the stop reply returns, plus `end_reason`. **A file with no summary line ended abruptly and is a partial capture** |

## Recipes

```sh
F=path/to/profile-*.jsonl

# The ten worst frames, with when they happened
jq -s '[.[]|select(.type=="frame")]|sort_by(-.frame_ms)[:10]|map({n,t_ms,frame_ms,script_ms})' $F

# Whole-window top functions (names included, no sig lookup needed)
jq 'select(.type=="total").funcs[:15]' $F

# One bad frame's own function rows, ids resolved by hand
jq 'select(.type=="frame" and .n==1234).funcs' $F
jq 'select(.type=="sig" and .id==7)' $F

# A single function's self-time over the window (by sig id)
jq -c 'select(.type=="frame") | {n, cost: (.funcs[]|select(.[0]==7)|.[2])}' $F

# Memory and VRAM as a time series; then the leak question in one line
jq -c 'select(.type=="mon")|{t_ms,static_mem,video_mem}' $F
jq -s '[.[]|select(.type=="mon").static_mem] | {start: first, end: last, delta: (last-first)}' $F

# GPU: the worst rendered frame and its passes; mean cost per pass
jq -s '[.[]|select(.type=="gpu")]|sort_by(-.total_gpu_ms)[0]' $F
jq -s '[.[]|select(.type=="gpu").areas[]] | group_by(.[0]) | map({pass: .[0][0], mean_gpu_ms: (map(.[2])|add/length)}) | sort_by(-.mean_gpu_ms)' $F

# The heaviest VRAM users at the end of the window
jq 'select(.type=="vram" and .phase=="end").resources[:15]' $F

# Did anything interfere with the capture?
jq 'select(.type=="event")' $F
```

## Caveats worth knowing before arguing from the data

- `frame.funcs` is a per-frame *top slice*, not the census; the census is `total`.
- `mon.raw` time monitors are seconds; the converted headline fields are ms. Index
  `raw` with `header.builtin_monitors`, and anything past that list with the latest
  `mon_names` record.
- GPU numbers describe the most recently *completed* profile; match them to CPU
  frames by `t_ms` proximity, not by equating `rs_frame` with `frame.n`.
- `summary.top_functions.source == "per_frame_fold"` means the accumulated total
  never arrived (the game usually died first) — treat its rows as a floor, not a
  census.
- A capture with `events > 0` had outside interference; read the `event` records
  before trusting the window's continuity.
