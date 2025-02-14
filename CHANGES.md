Version 2.0.0 in Progress

* Fix packet scheduling from orbuculum
* Move to semantic numbering - 2.00 becomes 2.0.0. 
* Various fixes in orbmortem for reliability and performance
* Improve timing of orbuculum mux to ensure data get flushed to orbmortem on crash
* Version bumped to 2.00 in Progress because of size and impact of changes
* Addition of `orbprofile` for ETM profiling and integration with {k/q}cachegrind, see https://github.com/KDE/kcachegrind.
* Addition of `orbmortem` for ETM/ETB post-mortem analysis.
* Licenses updated to use SPDX identifiers...the license itself hasn't changed, we just lost a chunk of text at the head of every file, with the license now in the LICENSE file.
* Orbtrace split off into separate repository
* Split of orbuculum into the network server (orbuculum) and the fifo server (orbfifo).
* Extensive changes to support orbtrace

3rd June 2022 (Version 1.13)

* Fix -v missing parameter
* Allow orbcat sockets to reconnect on loss of connection
* Add SAM5X SWO support
* Prevent segfault in orbtop when elf changes
* Fix assert when fpga has no data to deliver
* Add pyocd to list of trace sources
*Complete fix for specified length integers on 32 bit platforms (Rasbian support)
* Compatability with languages other than C family (specifically Rust)

23rd October 2020 (Version 1.10)

* Replace `master` with `main`.
* Single entry into a channel definition can be expanded multiple times (up to 4), so -c,z,"[%02x] %c" would print both a hex and ascii representation of a character, for example
* Link Monitoring and reporting (enabled with the `-m` option to orbuculum.
* Simple colour support (disable by commenting out `SCREEN_HANDLING` in the makefile).
* Internal restructuring to simplify the packet decode. This will help you if you want to implement your own handlers. See `orbcat.c` for a simple example, or `orbtop.c` if timestamp ordering is important to you.
* orbuculum now processes simple ISYNC messages, reporting them as type 8 in the hwevent fifo.
* ocbcat can now read directly from a file.
* orbcat and orbuculum can both terminate reading from a file when it's exhaused with the `-e` option.
* Addition of JSON output for orbtop
* Interrupt measurements have been added into orbtop
* Per interval status reporting (overflow, sync etc) has been
  added to the orbtop interactive display.
* povray splash screen generator
* Refactoring of fifo code to allow it to be optional in orbuculum.c

9th September 2019 (Version 1.00)

* Change to BSD from GPL License
* Fixes to command line options, raw output and HW event decoding
* Use of nextpnr-ice40 instead of arachne-pnr
* Extensive changes to gdbtrace.init

14th August 2019 (Version 0.23)

* Small edits to gdbtrace.init for tidying purposes
* Fix integration time units in orbtop
* Allow orbtop to gracefully restart when elf file is changed (e.g. on recompile)

13th August 2019 (Version 0.22 - no functional changes)

* Makefile/Include path change to allow compilation on OpenSuse

5th August 2019 (Version 0.22)

* Internal simplifications and tidying
* Fix signal bug which could lead to orbuculum bailing when a client disconnected

4th August 2019 (Version 0.21)

* gdbinit is changed to reference SWO rather than SWD (Issue #22)
* Support for higher speed connections using linux kernels that support the BOTHER option.
* Addition of C++ demangling support.
* Specific gdbinit support for STM32F4
* Small changes to support libftdi1.4 exclusively. It's essential you use this version of libftdi or higher!
