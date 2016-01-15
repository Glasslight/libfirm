# libfirm Makefile
#
# This is currently experimental and not fully supported, but we plan to replace
# the jambuild with this at some point in the future.
#
# Most variable names are similar to the names used by autoconf...
-include config.mak

# Some build configuration defaults
top_srcdir   ?=
top_builddir ?= build
host         ?= unknown-host
variant      ?= debug

srcdir       ?= $(top_srcdir)
builddir     ?= $(top_builddir)/$(variant)
docdir       ?= $(top_builddir)/doc

# This hides the noisy commandline outputs. You can see them with "make Q="
Q ?= @

CC ?= cc
DOXYGEN ?= doxygen
LINK ?= $(CC)
AR ?= ar
DLLEXT ?= .so

# Variants
CFLAGS_all        = -std=gnu99 -fPIC -DHAVE_FIRM_REVISION_H
CFLAGS_debug      = $(CFLAGS_all) -O0 -g3 -DDEBUG_libfirm
CFLAGS_profile    = $(CFLAGS_all) -O3 -pg -DNDEBUG -fno-inline
CFLAGS_coverage   = $(CFLAGS_all) -O0 --coverage -DDEBUG_libfirm
LINKFLAGS_profile  = -pg
LINKFLAGS_coverage = --coverage
CFLAGS_optimize   = $(CFLAGS_all) -O3 -fomit-frame-pointer -DNDEBUG

# General flags
CFLAGS    += $(CFLAGS_$(variant))
CFLAGS    += -Wall -W -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings
LINKFLAGS += $(LINKFLAGS_$(variant)) -lm
VPATH = $(srcdir)

REVISION ?= $(shell git describe --abbrev=40 --always --dirty --match '')

# Update revision.h if necessary
UNUSED := $(shell \
	REV="\#define libfirm_VERSION_REVISION \"$(REVISION)\""; \
	echo "$$REV" | cmp -s - firm_revision.h 2> /dev/null || echo "$$REV" > firm_revision.h \
)

all: firm
.PHONY: all

# disable make builtin suffix rules
.SUFFIXES:

# This rule is necessary so that make does not abort if headers get deleted
# (the deleted header might still be referenced in a .d file)
%.h:
	@:

# A very naive way to create a config.h if it is missing
$(srcdir)config.h:
	@echo MakeConfig $@
	$(Q)rm -f $@
	$(Q)echo "#define libfirm_VERSION_MAJOR 1" >> $@
	$(Q)echo "#define libfirm_VERSION_MICRO 0" >> $@
	$(Q)echo "#define libfirm_VERSION_MINOR 20" >> $@

# libFirm
libfirm_DIRS := \
	ir         \
	ir/adt     \
	ir/ana     \
	ir/arch    \
	ir/common  \
	ir/debug   \
	ir/obstack \
	ir/ident   \
	ir/net     \
	ir/ir      \
	ir/lower   \
	ir/libcore \
	ir/lpp     \
	ir/opt     \
	ir/st      \
	ir/stat    \
	ir/tr      \
	ir/tv      \
	ir/kaps    \
	ir/be
libfirm_SOURCES  = $(foreach dir,$(libfirm_DIRS),$(wildcard $(dir)/*.c))
libfirm_a        = $(builddir)/libfirm.a
libfirm_dll      = $(builddir)/libfirm$(DLLEXT)
libfirm_CPPFLAGS = -Iinclude/libfirm -Iinclude/libfirm/adt -I. $(foreach dir,$(libfirm_DIRS),-I$(dir))

.PHONY: firm
firm: $(libfirm_dll)

# backends
backends = amd64 arm ia32 sparc TEMPLATE

EMITTER_GENERATOR = $(srcdir)ir/be/scripts/generate_emitter.pl
EMITTER_GENERATOR2 = $(srcdir)ir/be/scripts/generate_emitter_new.pl
REGALLOC_IF_GENERATOR = $(srcdir)ir/be/scripts/generate_regalloc_if.pl
OPCODES_GENERATOR = $(srcdir)ir/be/scripts/generate_new_opcodes.pl

define backend_template
$(1)_SOURCES = $$(wildcard ir/be/$(1)/*.c)
$(1)_SOURCES := $$(filter-out ir/be/$(1)/gen_%.c, $$($(1)_SOURCES))
$(1)_GEN_HEADERS =

$(1)_SPEC = ir/be/$(1)/$(1)_spec.pl

$$(srcdir)ir/be/$(1)/gen_$(1)_emitter.h $$(srcdir)ir/be/$(1)/gen_$(1)_emitter.c: $$($(1)_SPEC) $$(EMITTER_GENERATOR) $(EMITTER_GENERATOR2)
	@echo GEN $$@
	$(Q)$$(EMITTER_GENERATOR) $$($(1)_SPEC) $$(srcdir)ir/be/$(1)
$(1)_SOURCES += ir/be/$(1)/gen_$(1)_emitter.c
$(1)_GEN_HEADERS += ir/be/$(1)/gen_$(1)_emitter.h

$$(srcdir)ir/be/$(1)/gen_$(1)_regalloc_if.h $$(srcdir)ir/be/$(1)/gen_$(1)_regalloc_if.c: $$($(1)_SPEC) $$(REGALLOC_IF_GENERATOR)
	@echo GEN $$@
	$(Q)$$(REGALLOC_IF_GENERATOR) $$($(1)_SPEC) $$(srcdir)ir/be/$(1)
$(1)_SOURCES += ir/be/$(1)/gen_$(1)_regalloc_if.c
$(1)_GEN_HEADERS += ir/be/$(1)/gen_$(1)_regalloc_if.h

$$(srcdir)ir/be/$(1)/gen_$(1)_new_nodes.h $$(srcdir)ir/be/$(1)/gen_$(1)_new_nodes.c.inl: $$($(1)_SPEC) $$(OPCODES_GENERATOR)
	@echo GEN $$@
	$(Q)$$(OPCODES_GENERATOR) $$($(1)_SPEC) $$(srcdir)ir/be/$(1)
$(1)_GEN_HEADERS += ir/be/$(1)/gen_$(1)_new_nodes.h

ir/be/$(1)/$(1)_new_nodes.c: ir/be/$(1)/gen_$(1)_new_nodes.c.inl

# We need to inform make of the headers it doesn't know yet...
$(1)_OBJECTS = $$($(1)_SOURCES:%.c=$$(builddir)/%.o)
$$($(1)_OBJECTS): $$($(1)_GEN_HEADERS)

libfirm_SOURCES += $$($(1)_SOURCES)
libfirm_DIRS += ir/be/$(1)
endef

$(foreach backend,$(backends),$(eval $(call backend_template,$(backend))))

# generators
IR_SPEC_GENERATED_FILES := \
	include/libfirm/nodeops.h \
	include/libfirm/opcodes.h \
	ir/ir/gen_ir_cons.c.inl   \
	ir/ir/gen_irop.c.inl      \
	ir/ir/gen_irnode.c.inl    \
	ir/ir/gen_irnode.h
IR_SPEC_GENERATOR := scripts/gen_ir.py
IR_SPEC := scripts/ir_spec.py

$(IR_SPEC_GENERATED_FILES): $(IR_SPEC_GENERATOR) $(IR_SPEC) scripts/spec_util.py
	@echo GEN $@
	$(Q)$(IR_SPEC_GENERATOR) $(IR_SPEC) ir/ir

IR_IO_GENERATOR := scripts/gen_ir_io.py
IR_IO_GENERATED_FILES := ir/ir/gen_irio.inl

$(IR_IO_GENERATED_FILES): $(IR_IO_GENERATOR) $(IR_SPEC) scripts/spec_util.py
	@echo GEN $@
	$(Q)$(IR_IO_GENERATOR) $(IR_SPEC) ir/ir

ir/ir/irio.c: $(IR_IO_GENERATED_FILES)

libfirm_OBJECTS = $(libfirm_SOURCES:%.c=$(builddir)/%.o)
libfirm_DEPS    = $(libfirm_OBJECTS:%.o=%.d)
-include $(libfirm_DEPS)

$(libfirm_a): $(libfirm_OBJECTS)
	@echo AR $@
	$(Q)$(AR) -crs $@ $^

$(libfirm_dll): $(libfirm_OBJECTS)
	@echo LINK $@
	$(Q)$(LINK) -shared $(LINKFLAGS) -o $@ $^

# Generic rules
UNUSED := $(shell mkdir -p $(libfirm_DIRS:%=$(builddir)/%))
$(builddir)/%.o: %.c $(IR_SPEC_GENERATED_FILES) config.h
	@echo CC $@
	$(Q)$(CC) $(CFLAGS) $(CPPFLAGS) $(libfirm_CPPFLAGS) -MMD -c -o $@ $<

$(docdir)/libfirm.tag: $(IR_SPEC_GENERATED_FILES) Doxyfile $(wildcard include/libfirm/*.h) $(wildcard include/libfirm/adt/*.h)
	@echo Doxygen $@
	$(Q)$(DOXYGEN)

DOCU_GENERATOR := scripts/gen_docu.py
$(docdir)/html/nodes.html: $(docdir)/libfirm.tag $(DOCU_GENERATOR) $(IR_SPEC) scripts/spec_util.py scripts/style.css
	@echo gen_docu.py $@
	$(Q)$(DOCU_GENERATOR) $(IR_SPEC) $(docdir)/libfirm.tag "" $@
	$(Q)cp scripts/style.css $(docdir)/html

.PHONY: doc
doc: $(docdir)/libfirm.tag $(docdir)/html/nodes.html

.PHONY: clean
clean:
	@echo CLEAN
	$(Q)rm -f $(libfirm_OBJECTS)
	$(Q)rm -f $(libfirm_TARGET)
	$(Q)rm -f $(shell find ir/ -name "gen_*.[ch]")
