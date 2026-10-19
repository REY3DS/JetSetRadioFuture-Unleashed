# The JSRF-specific half

Everything in this repository except this folder and `launcher/` is
title-agnostic machinery — the kernel layer, the NV2A and MCPX models, the
recompilation pipeline. These two files are the part that is about *Jet Set
Radio Future* and nothing else.

- **`main.c`** — the port's entry point. Loads the XBE for its data sections,
  maps the Xbox address space, starts the kernel and the hardware models, and
  calls the recompiled entry point. It knows the game's entry-point address
  and where to find its files.

- **`recomp_manual.c`** — hand-written overrides for individual recompiled
  functions, by guest address. Some are fixes for functions the lifter got
  wrong; most are instrumentation that answers a specific question about a
  specific fault, kept because the question tends to come back. The comments
  say which fault each one was written for.

The bulk of the port — the hundreds of thousands of lines of C the pipeline
generates from the XBE — is **not** here and will not be: it is derived from a
copyrighted binary. It is produced on your machine, from your own disc, by the
pipeline in `tools/`.

`CMakeLists.txt` here builds the game from this repository once the generated
code is in `port/recomp/gen` (see the main README), and stamps it with the
version of the git tag the tree is at.

## Regenerating the generated half

The generated C is derived from the game's executable and is not in git. Put
your own `default.xbe` in a directory (for example `~/jsrf-game`) and run:

```sh
python3 port/regenerate.py --game-dir ~/jsrf-game
```

The script runs the disassembler, function identification, ABI analysis and
recompiler with the seed sets in `port/seeds/`, then runs
`port/postprocess_gen.py`. That last step wires the hand-written wrappers that
the public recompiler leaves pointing at the raw `_gen` bodies -- currently the
NV2A PGRAPH interrupt (`0x00194210`) and the DirectSound DSP doorbell
(`0x001A1769`) -- and writes the empty `recomp_tables_stub.c` that
`recomp_manual.c` needs to link. The generated folder is ignored by git.

The seeds are the filtered ICALL feedback from a run that reached gameplay,
collected from the retail XBE this port was developed against; a different
revision may need its own feedback pass. They are cumulative: when a later
run discovers a new target, merge its `icall_targets.dump` and regenerate the
seed file with `tools.recomp.icall_feedback` (see
`docs/technical/indirect-calls.md`). Do not hand-edit the generated C; a
fresh run overwrites it.
