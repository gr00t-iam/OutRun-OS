# demo.mk — a real project, built on OutRun OS by OutRun OS.
#
# Nothing here runs on a host. `omake -f /src/demo.mk` reads this file on the
# running system, resolves the graph, and forks /bin/occ for the parts that are
# out of date. Try it twice: the second run reports the target up to date,
# because the fold of the prerequisites' content hashes still matches the
# signature recorded in the stamp file.
#
#   omake -f /src/demo.mk              build the default goal (all)
#   omake -f /src/demo.mk /bin/hello.elf   build one target by name

CC  = occ
HDR = /src/hello.h
SRC = /src/hello_a.c /src/hello_b.c
OUT = /bin/hello.elf

# The first rule is the default goal. It has prerequisites and no recipe, which
# is the ordinary way to write an aggregate goal.
all: $(OUT)

$(OUT): $(SRC) $(HDR)
	echo building $(OUT)
	$(CC) $(SRC) -o $(OUT)
