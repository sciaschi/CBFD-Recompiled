# Save states

Save states let you save the game exactly as it is, anywhere, and go back to that
moment later. They're useful for the long, tricky parts of the game, where a
mistake would otherwise send you back to the last checkpoint.

The first half of this page is for players. The second half explains how save
states work in a recompiled game. As far as we know, this is the first N64
recompilation to have them, so it's written for other recomp developers to
build on.

## Using save states

| Key | What it does |
|---|---|
| **F5** | Save the game to the current slot |
| **F7** | Load the current slot |
| **F6** | Switch to the next slot (1 to 9, then back to 1) |

A message in the top-left corner confirms each one: "State saved to slot 1",
"Loaded the state in slot 1", "Save state slot 2".

- **Slots.** There are nine. Saving to a slot replaces what was in it. The
  game always starts on slot 1.
- **Load anywhere.** You can load a state from anywhere in the game, not just
  where you saved it. That includes the title screen right after starting the
  program.
- **Your real saves are safe.** The game's own save file (the one the game
  writes at checkpoints) is never changed by saving or loading a state, so a
  state can't erase your progress. It also means that loading a state doesn't
  take back anything the game has saved since.
- **Where they're kept.** Each slot is a file of about 8 MB, `slot1.state` to
  `slot9.state`, in the `savestates` folder next to your settings:
  `%LOCALAPPDATA%\ConkerRecompiled\savestates` on Windows,
  `~/.config/ConkerRecompiled/savestates` on Linux,
  `~/Library/Application Support/ConkerRecompiled/savestates` on macOS (or
  next to the program, in portable mode). You can copy them, back them up or
  delete them.

### Things to know

- **States belong to the version of the program that made them.** After you
  update Conker's Bad Fur Day: Recompiled (or switch to a test build), states
  made with the other one can't be loaded. You'll see "it was saved by another
  version or build of the program". Keep a normal save for anything you care
  about.
- **RetroAchievements.** States work with RetroAchievements in casual mode,
  and the achievements' progress is saved and restored with them. Loading is
  turned off in Hardcore mode, as RetroAchievements' rules require.
- **A state is taken between frames**, at a moment when the game isn't in the
  middle of something. That's almost always within a frame of pressing the key.
  If the game stays busy (rarely), you'll see "Couldn't save now" or "Couldn't
  load right now". Just press the key again.
- **Mods.** The base game and the included mods (Skip Intro, Skip Any
  Cutscene, Cheats) are covered. A code mod that keeps its own data in the
  mods' memory doesn't have that data saved.
- The sound may skip for a moment as a state loads.

## How it works

### Why recompiled games don't have save states

An emulator runs the whole N64 as data: the console's memory and the CPU's
registers. Saving a state means copying that data, and loading it means
copying it back.

A recompiled game is different. The game's MIPS code has been translated into
C and built into a normal program, and each of the game's threads (the N64's
operating system, libultra, runs several) is a real thread of that program.
Where each thread is in the game's code, meaning which function called which,
lives on that thread's native call stack. A native stack can't be written to a
file and rebuilt later: it holds return addresses into the program, the
compiler's saved registers, and so on.

### The idea: only save when every thread is somewhere known

Two facts make it possible anyway.

1. **The game's threads take turns.** N64ModernRuntime runs them like the N64
   does: one at a time. A thread runs until it waits (for a message, to send
   one, or to be started), then the next one runs.
2. **Everything else a thread has is plain data.** The recompiled code keeps
   the MIPS registers in a context struct (`recomp_context`) rather than in
   native variables, and the MIPS stack is in the game's memory. The native
   stack only says *where* the thread is.

So if a thread waits at the same place, with the same chain of calls, each
time, then whatever its native stack holds there is the same each time too.
Only its registers and the game's memory differ. That's a state that can be
saved and loaded.

A state is taken at a **safe point**: a moment when every game thread but one
is waiting, and the running one is at a known call itself. There are two kinds:

- the **idle thread** (libultra's lowest priority thread) in `pause_self`,
  which only runs when every other thread waits;
- **any thread as it checks a message queue** (`osRecvMesg`, waiting or not)
  while no other thread is ready to run. Some games poll a queue and never let
  the idle thread run, so this one is needed too.

A state is loaded at a safe point where every thread is where it was when the
state was taken. In practice games wait at a few fixed places in their main
loops, so Conker's threads match within a frame, even across levels, menus
and restarts of the program. That's why a state can be loaded anywhere.

### Telling where a thread is: its signature

Each time a thread is about to wait, the runtime records its **signature**: a
hash of its native call stack. The stack is walked with
`RtlCaptureStackBackTrace` on Windows and `backtrace()` on Linux and macOS.
Only the frames inside the program itself count, as offsets from where it's
loaded. That way the signature is the same every time the same build runs,
whatever address the system loads the program at. Frames in system libraries
(the start of a thread) are left out, since their addresses change from run
to run.

The values a message queue wait holds in its own native frame (the queue, the
message or where it goes, and the flags) are hashed in too, because they
aren't in the registers.

A load only goes ahead if the program has the same set of threads as the
state, and each one has the same signature. Otherwise it waits for the next
safe point and checks again, for up to five seconds.

Because signatures are offsets into the program's code, a state only fits the
build that took it: any rebuild that changes the code moves them. So each
state also records a **build fingerprint** (where a few of the runtime's
functions are in the program, and the program's size). A state from another
build is only tried for a second, and the message says why it can't be
loaded.

For Conker, the threads wait 7 to 12 native frames deep, at the same places
every frame.

### What a state holds

| Part | From | Notes |
|---|---|---|
| The game's memory | librecomp | The N64's 8 MB, including every MIPS stack, `OSThread`, message queue and timer |
| Each thread's registers | librecomp | `r0`-`r31`, `f0`-`f31`, `hi`, `lo`, the status register and the float mode. `f_odd` (a pointer into the context) is recomputed on load |
| Each thread's signature | ultramodern | Checked before loading, never written back |
| The clock and the timers | ultramodern | The game's clock (`osGetCount`/`osGetTime`) and the list of running timers |
| The VI and event state | ultramodern | The video mode, framebuffers, retrace message and rate, and the SP/DP/AI/SI event queues |
| RetroAchievements progress | the host | `rc_client_serialize_progress` |

A few details make loading work:

- **Native pointers in game memory.** Each `OSThread` in game memory holds a
  pointer to its native `UltraThreadContext`. After the memory is copied in,
  each thread's pointer is put back to the current one; otherwise a resumed
  thread would find a stale pointer and destroy itself.
- **The clock.** The game's clock is the time since the program started,
  minus an offset. On load, the offset is set so the clock carries on from the
  state's time. The timers' deadlines and the timestamps the game keeps are on
  that clock, so they stay correct, even after a restart.
- **The timers.** The timer thread now keeps its running timers in a plain
  list under a lock. It used to keep a set ordered by deadlines that live in
  game memory, which a load would scramble. Saving and loading hold that lock.
- **Nothing in flight.** A state is only taken or loaded when no RSP task is
  queued or running (each counts as in flight until its last message is sent)
  and no event message is waiting to be handed to the game. Event messages that
  arrive for the replaced state while a load is happening are dropped. The next
  VI starts the loaded state off.

The game's EEPROM (its real save) is left out on purpose.

### Where the code is

| Where | What |
|---|---|
| `ultramodern/include/ultramodern/save_states.hpp`, `ultramodern/src/save_states.cpp` | The thread list, signatures and safe points |
| `ultramodern/src/threads.cpp`, `mesgqueue.cpp`, `scheduling.cpp` | Record each wait. Safe points in `osRecvMesg` and `pause_self` |
| `ultramodern/src/timer.cpp` | The clock offset, and the timer thread's list under a lock |
| `ultramodern/src/events.cpp` | The VI and event state; RSP tasks in flight |
| `librecomp/include/librecomp/save_states.hpp`, `librecomp/src/save_states.cpp` | `capture` and `restore`, and the state's layout |
| `librecomp/src/recomp.cpp` | Registers each thread's `recomp_context` |
| `host/src/save_states.cpp` | Conker's keys, slots, files and messages |

The N64ModernRuntime changes are in
[`recomp/n64modernruntime.patch`](../recomp/n64modernruntime.patch), which the
build applies.

### Adding save states to another recomp

The runtime part (the two `save_states` libraries and the small hooks above)
doesn't depend on Conker. Another recomp built on N64ModernRuntime needs:

1. The runtime changes from the patch.
2. A safe point callback (`ultramodern::save_states::set_safe_point_callback`)
   that calls `recomp::save_states::capture` or `restore` while a request is
   pending. Turn the safe points on with `set_safe_points_wanted(true)` only
   while one is pending: each one walks a stack, and a game can check a queue
   thousands of times a second.
3. Its own files, keys and messages (Conker's are in `host/src/save_states.cpp`).

What to check for a new game:

- **Threads that wait in many different places.** Those still save, but loads
  only succeed where the threads line up. The temporary log (see below) shows
  each thread's signature at each save and failed load.
- **Native state outside the registers.** Conker's script interpreter uses
  `setjmp`/`longjmp`, and its jump buffer lives in the program. That's fine as
  long as no safe point falls inside the interpreter, which the signatures
  ensure. A game whose patches keep values in static native variables needs to
  save those too.
- **Memory past the N64's 8 MB** (the mods' heap) isn't saved.
- **Host code with its own state** (Conker's camera and music fixes, for
  example) carries on as it was. That has been harmless so far.

### Debugging

While save states are new, each save and load is logged to
`savestates_log.txt`, next to the settings. The log records which thread was
running, each thread's signature and stack depth, and why a load couldn't be
done yet.
