# SPDX-License-Identifier: GPL-2.0-only
# Encore FAS (https://github.com/rem01project/encore_fas, commit f408a00),
# built in. The upstream Makefile probes the uprobe/hrtimer API from the
# headers; this tree is android16-6.12: uprobe API 2, no hrtimer_setup,
# soft hrtimer modes present.
obj-$(CONFIG_ENCORE_FAS) += encore_fas.o
encore_fas-y := encore_fas_main.o \
                encore_fas_ctx.o \
                encore_fas_evq.o \
                encore_fas_time.o

ccflags-y += -I$(src)/include -DFAS_VERSION=9 -DFAS_BRANCH=\"main-f408a00\"
ccflags-y += -DFAS_UPROBE_API=2 -DFAS_HAVE_HRTIMER_SOFT
