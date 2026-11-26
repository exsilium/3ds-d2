// The natives' sites (issue #138, docs/aot.md "The natives' site table"):
// every change a native makes to the game's code, in one table that the
// install code (native_sites.cpp) and the AOT recompiler
// (tools/aot/recompile, which compiles the code as changed) both read.
//
// Included with the three macros defined; the recompiler reads these lines
// as text, so each row stays on one line, with plain literals:
//
//   NATIVE(name, stack_dwords)   a native the sites call: rt::native_entry's
//                                name and the dwords its ret pops
//   GROUP(name, compiled)        sites applied together, by the install code
//                                that owns them; compiled 1: the recompiler
//                                compiles the game's code with them
//   SITE(group, va, "game bytes", "bytes written")
//
// Bytes are hex pairs. In the bytes written, {Name} is the rel32 to that
// native (4 bytes) and {=0x...} the rel32 to that address in the game's code.
// Nothing written depends on the run: a value the run chooses goes in data
// the site's code reads.

#ifndef NATIVE
#define NATIVE(name, stack_dwords)
#define GROUP(name, compiled)
#define SITE(group, va, game_bytes, bytes_written)
#define NATIVE_SITES_TABLE_DEFAULTS
#endif

// dcc_native.cpp: 0x60bff0's per-direction loop, 0x60c10a (21 bytes), becomes
// a call to the native decoder and the loop's compare.
NATIVE(DccDecode, 0)
GROUP(dcc, 1)
SITE(dcc, 0x0060c10a, "8b 45 14 3b 85 c0 fc ff ff 89 b5 8c fc ff ff 89 b5 d0 fc ff ff", "e8 {DccDecode} 3b 85 c0 fc ff ff 90 90 90 90 90 90 90 90 90 90")

// d2_memory.cpp, install_sprite_frames: the sprites' frames in blocks of
// their own (issue #44). 0x60bff0's call to 0x40b430 at 0x60c103 asks a
// native for the group's frames. 0x409ab0 (a pool's free) sends a pointer in a
// 64 KiB granule the frames hold (the byte map at 0x6cb8c0; the granules'
// base and length in the data dwords at 0x6cb8b8 and 0x6cb8bc) to a native.
// 0x409c80 (a pool system's destroy) tells a native first. 0x5ff6e0 counts a
// decoded group as the block its frames hold (0x5ff72d), and 0x5fffa0 a DCC
// file's bytes as none (0x60002d). The stubs are in .text's zero slack.
NATIVE(SpriteFramesAlloc, 3)
NATIVE(SpriteFramesFree, 1)
NATIVE(SpriteFramesPoolGone, 1)
NATIVE(SpriteFramesHeld, 2)
GROUP(sprite_frames, 1)
SITE(sprite_frames, 0x0060c103, "e8 28 f3 df ff", "e8 {SpriteFramesAlloc}")
SITE(sprite_frames, 0x006cb820, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "8b 02 2b 05 b8 b8 6c 00 3b 05 bc b8 6c 00 73 1e c1 e8 10 80 b8 c0 b8 6c 00 00 74 12 51 52 ff 32 e8 {SpriteFramesFree} 5a 59 85 c0 74 03 c2 08 00 55 8b ec 51 53 e9 {=0x409ab5}")
SITE(sprite_frames, 0x006cb860, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "51 52 ff 74 24 0c e8 {SpriteFramesPoolGone} 5a 59 55 8b ec 83 ec 08 e9 {=0x409c86}")
SITE(sprite_frames, 0x006cb880, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "ff 76 18 ff 76 14 e8 {SpriteFramesHeld} 6a 00 50 e9 {=0x5ff733}")
SITE(sprite_frames, 0x006cb8a0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "50 8b 45 08 f6 00 02 58 74 02 33 c0 50 e8 {=0x5ff630} e9 {=0x600033}")
SITE(sprite_frames, 0x00409ab0, "55 8b ec 51 53", "e9 {=0x6cb820}")
SITE(sprite_frames, 0x00409c80, "55 8b ec 83 ec 08", "e9 {=0x6cb860} 90")
SITE(sprite_frames, 0x005ff72d, "8b 4e 18 6a 00 51", "e9 {=0x6cb880} 90")
SITE(sprite_frames, 0x0060002d, "50 e8 fd f5 ff ff", "e9 {=0x6cb8a0} 90")

// d2_memory.cpp, install_sprite_decode_timer: 0x5ff6e0 (a sprite's decode
// into the cache, ret 0xc) timed by two natives around it. Its stub calls the
// function's first bytes, moved to their own row after it, which jump to the
// rest (the recompiler compiles that call's target as a function).
NATIVE(SpriteDecodeBegin, 0)
NATIVE(SpriteDecodeEnd, 0)
GROUP(decode_timer, 1)
SITE(decode_timer, 0x006cb740, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "50 51 52 e8 {SpriteDecodeBegin} 5a 59 58 ff 74 24 0c ff 74 24 0c ff 74 24 0c e8 {=0x6cb774} 50 51 52 e8 {SpriteDecodeEnd} 5a 59 58 c2 0c 00")
SITE(decode_timer, 0x006cb774, "00 00 00 00 00 00 00 00 00 00", "55 8b ec 53 56 e9 {=0x5ff6e5}")
SITE(decode_timer, 0x005ff6e0, "55 8b ec 53 56", "e9 {=0x6cb740}")

// d2_memory.cpp, install_pool_release: 0x40a080 (the pool system's
// allocation) frees the blocks a native hands it first, while the flag byte
// at 0x6cb6a0 is set: VirtualFree and free (0x683fc0) for each.
NATIVE(PoolReleaseNext, 0)
GROUP(pool_release, 1)
SITE(pool_release, 0x006cb660, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "80 3d a0 b6 6c 00 00 74 28 50 51 52 e8 {PoolReleaseNext} 85 c0 74 19 52 68 00 80 00 00 6a 00 50 ff 15 20 c2 6c 00 e8 {=0x683fc0} 83 c4 04 eb de 5a 59 58 55 8b ec 53 8b d9 e9 {=0x40a086}")
SITE(pool_release, 0x0040a080, "55 8b ec 53 8b d9", "e9 {=0x6cb660} 90")

// d2_memory.cpp, set_pool_block_size: 0x409dd0's blocks of 0x80000 bytes
// (mov eax, 0x80000 at 0x409fa8) become the data dword's at 0x6cb6a4, or one
// unit where a unit is larger.
GROUP(pool_block_size, 1)
SITE(pool_block_size, 0x006cb6b0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "a1 a4 b6 6c 00 3b f0 7e 02 8b c6 e9 {=0x409fad}")
SITE(pool_block_size, 0x00409fa8, "b8 00 00 08 00", "e9 {=0x6cb6b0}")

// d2_memory.cpp, install_sprite_cache_counter: 0x5ff3f0 counts a sprite into
// the cache at 0x5ff476; a native counts it too.
NATIVE(SpriteCacheInsert, 0)
GROUP(sprite_cache_counter, 1)
SITE(sprite_cache_counter, 0x006cb6c0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "e8 {SpriteCacheInsert} 8b 55 08 83 47 0c 01 01 57 08 e9 {=0x5ff480}")
SITE(sprite_cache_counter, 0x005ff476, "8b 55 08 83 47 0c 01 01 57 08", "e9 {=0x6cb6c0} 90 90 90 90 90")

// d2_memory.cpp, install_fatal_exit: exit (0x681e09) with -1 goes to a native
// that ends the run; any other code runs exit's body, moved to two runs of
// int3 padding (0x6814d1 and 0x681531).
NATIVE(FatalExit, 0)
GROUP(fatal_exit, 1)
SITE(fatal_exit, 0x006814d1, "cc cc cc cc cc cc cc cc cc cc cc cc cc", "6a 00 6a 00 ff 74 24 0c e9 {=0x681531}")
SITE(fatal_exit, 0x00681531, "cc cc cc cc cc cc cc cc cc", "e8 {=0x681d27} 83 c4 0c c3")
SITE(fatal_exit, 0x00681e09, "6a 00 6a 00 ff 74 24 0c e8 11 ff ff ff 83 c4 0c", "83 7c 24 04 ff 0f 85 {=0x6814d1} e9 {FatalExit}")

// automap.cpp, install_automap_capture: the UI pass's automap calls at
// 0x456f93 (in 0x456ee0, 23 bytes) become a call to a stub in .text's slack:
// a native decides (eax bit 0 the game's map, bit 1 the units), and the
// units' pass (0x457520, 0x45ac90) runs between two natives that capture it.
// 0x45a860 (one unit's cross and name) goes through a filter that skips the
// player's unit while the flag byte at 0x6cb640 is set.
NATIVE(AutomapDecide, 0)
NATIVE(AutomapBegin, 0)
NATIVE(AutomapEnd, 0)
GROUP(automap, 1)
SITE(automap, 0x006cb5b0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "e8 {AutomapDecide} 85 c0 74 31 50 e8 {=0x45ae90} 83 f8 03 74 25 f6 04 24 02 74 14 e8 {=0x457520} e8 {AutomapBegin} e8 {=0x45ac90} e8 {AutomapEnd} f6 04 24 01 74 05 e8 {=0x45ad60} 58 c3")
SITE(automap, 0x006cb600, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "80 3d 40 b6 6c 00 00 74 09 3b 05 70 6a 7a 00 75 01 c3 55 8b ec 83 ec 14 e9 {=0x45a866}")
SITE(automap, 0x0045a860, "55 8b ec 83 ec 14", "e9 {=0x6cb600} 90")
SITE(automap, 0x00456f93, "39 1d e8 27 7a 00 74 0f e8 f0 3e 00 00 83 f8 03 74 05 e8 b6 3d 00 00", "e8 {=0x6cb5b0} 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90")

// storm_reads.cpp, install_storm_read_priority: SFileReadFile's deadline for
// a queued read (push 0x7fffffff at 0x41aeb2) is 0 for a read without
// OVERLAPPED ([ebp+0x18] 0), so it is taken next.
GROUP(storm_priority, 1)
SITE(storm_priority, 0x006cb780, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "83 7d 18 00 75 07 6a 00 e9 {=0x41aeb7} 68 ff ff ff 7f e9 {=0x41aeb7}")
SITE(storm_priority, 0x0041aeb2, "68 ff ff ff 7f", "e9 {=0x6cb780}")

// storm_reads.cpp, set_storm_read_rate: the game sets the read thread's
// rate limit with push 0x40000; call 0x4164a0 at 0x40a3b8. The call goes to
// a native, which keeps the run's rate where Storm does.
NATIVE(StormRate, 1)
GROUP(storm_rate, 1)
SITE(storm_rate, 0x0040a3bd, "e8 de c0 00 00", "e8 {StormRate}")

// storm_reads.cpp, install_storm_direct_reads: the read thread sets a flag
// byte before it leaves Storm's lock to read (0x41a66c) and clears it after
// (0x41a725); SFileReadFile's check for an asynchronous archive (0x41ace2, 13
// bytes) asks a native, which sets ecx: 1 queues. The flag is in scratch
// memory the run allocates (a write to a page of code would throw its
// translations away); its address is in the data dword at 0x6cb7dc.
NATIVE(StormDirectRead, 0)
GROUP(storm_direct, 1)
SITE(storm_direct, 0x006cb7a0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "8b 1d dc b7 6c 00 c6 03 01 8b 1d 14 c2 6c 00 e9 {=0x41a672}")
SITE(storm_direct, 0x006cb7b8, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "50 a1 dc b7 6c 00 c6 00 00 58 83 7e 28 00 0f 84 {=0x41a763} e9 {=0x41a72b}")
SITE(storm_direct, 0x0041a66c, "8b 1d 14 c2 6c 00", "e9 {=0x6cb7a0} 90")
SITE(storm_direct, 0x0041a725, "83 7e 28 00 74 38", "e9 {=0x6cb7b8} 90")
SITE(storm_direct, 0x0041ace2, "83 ba 10 01 00 00 00 0f 85 04 01 00 00", "e8 {StormDirectRead} 85 c9 0f 85 {=0x41adf3}")

// frame_reads.cpp, install_frame_reads: SFileReadFile (0x41aad0) calls a
// native first, which names the game thread's read and clears *read as the
// 19 bytes replaced did (test eax, eax; je; mov dword [eax], 0), then makes
// its frame.
NATIVE(FrameRead, 0)
GROUP(frame_reads, 1)
SITE(frame_reads, 0x0041aad0, "55 8b ec 8b 45 14 83 ec 2c 85 c0 74 06 c7 00 00 00 00 00", "e8 {FrameRead} 55 8b ec 83 ec 2c 90 90 90 90 90 90 90 90")

// monster_pace.cpp, install_monster_pace: 0x46feb0's call of 0x46fda0 for
// a mode (0x46ff31, 9 bytes: mov edx, [ebp+8]; push edx; call) goes through
// a stub in .text's slack: a native gives the mode to go on with, or -1 for
// a mode left to a later walk. 0x470350 walks every 100 ms, not 600.
NATIVE(MonsterMode, 0)
GROUP(monster_pace, 1)
SITE(monster_pace, 0x006cb648, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "e8 {MonsterMode} 83 f8 ff 74 08 ff 75 08 e8 {=0x46fda0} e9 {=0x46ff3a}")
SITE(monster_pace, 0x0046ff31, "8b 55 08 52 e8 66 fe ff ff", "e9 {=0x6cb648} 90 90 90 90")
SITE(monster_pace, 0x0047036b, "81 c1 58 02 00 00", "81 c1 64 00 00 00")

// player_modes.cpp, install_player_modes: 0x470350's call of the walk of
// the units near the player (0x470070) goes to a native, which makes the
// call and then asks for one of the player's modes.
NATIVE(PlayerModes, 0)
GROUP(player_modes, 1)
SITE(player_modes, 0x00470377, "e8 f4 fc ff ff", "e8 {PlayerModes}")

// sprite_headers.cpp, install_sprite_headers: 0x5fe990 (a sprite's file
// record) has 0x60bf20 read a DCC file's header at 0x5fea79; a native fills
// the record from the headers kept, or has 0x60bf20 read it.
NATIVE(SpriteHeader, 0)
GROUP(sprite_headers, 1)
SITE(sprite_headers, 0x005fea79, "e8 a2 d4 00 00", "e8 {SpriteHeader}")

// sound_defer.cpp, install_sound_defer: 0x481950 (the sound tables' load)
// reads the record count at 0x482173, the records filled; a native returns
// it, and gives each record that isn't a stream sounds.txt's Defer Inst.
// 0x482b40 (the tick's scan of the records, which finds a load finished)
// asks a native at 0x482b64 whether it is due, in place of its 25 ticks; and
// natives count a load in (0x482b1d, in 0x482970) and out (0x481772, in
// 0x481720) as the game does, timing it.
NATIVE(SoundTable, 0)
NATIVE(SoundAsked, 0)
NATIVE(SoundLoaded, 0)
NATIVE(SoundScanDue, 0)
GROUP(sound_defer, 1)
SITE(sound_defer, 0x00482173, "a1 a4 c9 7b 00", "e8 {SoundTable}")
SITE(sound_defer, 0x00482b64, "8b c1 2b 05 c0 c9 7b 00 83 f8 19 0f 82 9d 00 00 00", "e8 {SoundScanDue} 83 f8 01 0f 82 {=0x482c12} 90 90 90")
SITE(sound_defer, 0x00482b1d, "01 3d c8 c9 7b 00", "e8 {SoundAsked} 90")
SITE(sound_defer, 0x00481772, "83 2d c8 c9 7b 00 01", "e8 {SoundLoaded} 90 90")

// cinematics.cpp, install_cinematics: the GDI renderer's three video entries
// (0x6c7cc0: "return 0", "ret 4", "return 0", 16 bytes each) become the play
// loop (fastcall: ecx the name, edx the mode, one stack argument popped):
// VideoOpen(name, mode), then VideoStep and Sleep (through the game's import
// slot, 0x6cc258) until the video ends. The renderer's table gets the loop
// and two natives in the three slots (data, written by the install code).
NATIVE(VideoOpen, 2)
NATIVE(VideoStep, 0)
GROUP(cinematics, 1)
SITE(cinematics, 0x006c7cc0, "33 c0 c3 cc cc cc cc cc cc cc cc cc cc cc cc cc c2 04 00 cc cc cc cc cc cc cc cc cc cc cc cc cc 33 c0 c3 cc cc cc cc cc cc cc cc cc cc cc cc cc", "52 51 e8 {VideoOpen} 85 c0 74 13 e8 {VideoStep} 83 f8 ff 74 09 50 ff 15 58 c2 6c 00 eb ed 31 c0 c2 04 00 cc cc cc cc cc cc cc cc cc cc cc cc cc")

// d2_memory.cpp, set_sprite_cache_floor: 0x457300's floor for the sprite
// cache (mov edx, 0x01800000 at 0x4573e7) becomes the data dword's at
// 0x6cb644, through a stub in .text's slack. Applied on every run: with no
// floor asked for, the dword holds the game's own.
GROUP(sprite_cache_floor, 1)
SITE(sprite_cache_floor, 0x006cb620, "00 00 00 00 00 00 00 00 00 00 00", "8b 15 44 b6 6c 00 e9 {=0x4573ec}")
SITE(sprite_cache_floor, 0x004573e7, "ba 00 00 80 01", "e9 {=0x6cb620}")

// d2_native.cpp, install_floor_queue: the world's draw in 0x476bc0 calls two
// natives where it called the floor's draw (0x476cdc, to 0x4ded10) and the
// function after it (0x476ce1, to 0x473c00); each goes on to the call's own
// target. Applied on every run: with the queue off they do nothing else.
NATIVE(FloorQueueArm, 0)
NATIVE(FloorQueueEnd, 0)
GROUP(floor_queue, 1)
SITE(floor_queue, 0x00476cdc, "e8 2f 80 06 00", "e8 {FloorQueueArm}")
SITE(floor_queue, 0x00476ce1, "e8 1a cf ff ff", "e8 {FloorQueueEnd}")

// sprite_trace.cpp, install_sprite_trace: the sprite cache's uses (0x600350,
// in 0x6001f0) and removals (0x5ff53c, in 0x5ff4a0) go through stubs in
// .text's zero slack, each: cmp byte [0x6cb6e0], 0 (the trace's flag); je
// over; push eax, ecx, edx; (the use: its argument, eax with bit 0 set when
// [ebp+0xc] is 0, pushed); call the native; pop them; over: the bytes the
// jump replaced; jmp back. Compiled with them, so the AOT build traces too.
NATIVE(SpriteTraceUse, 1)
NATIVE(SpriteTraceRemove, 0)
GROUP(sprite_trace, 1)
SITE(sprite_trace, 0x006cb6f0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "80 3d e0 b6 6c 00 00 74 1a 50 51 52 89 c1 83 7d 0c 00 0f 94 c2 0f b6 d2 09 d1 51 e8 {SpriteTraceUse} 5a 59 58 8b 58 20 85 db e9 {=0x600355}")
SITE(sprite_trace, 0x006cb720, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "80 3d e0 b6 6c 00 00 74 0b 50 51 52 e8 {SpriteTraceRemove} 5a 59 58 8b 4f 04 29 4e 08 e9 {=0x5ff542}")
SITE(sprite_trace, 0x00600350, "8b 58 20 85 db", "e9 {=0x6cb6f0}")
SITE(sprite_trace, 0x005ff53c, "8b 4f 04 29 4e 08", "e9 {=0x6cb720} 90")

#ifdef NATIVE_SITES_TABLE_DEFAULTS
#undef NATIVE
#undef GROUP
#undef SITE
#undef NATIVE_SITES_TABLE_DEFAULTS
#endif
