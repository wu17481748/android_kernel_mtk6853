# AW88263 codec driver

#MODULE_NAME = snd-soc-aw882xx
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/common

ccflags-y += -Isound/soc/codecs/aw882xx
ccflags-y += -DDEBUG
ccflags-y += -Werror
ccflags-y += $(call cc-option,-Wno-date-time,)

snd-soc-aw88263-objs := awinic_monitor.o awinic_dsp.o awinic_cali.o aw882xx.o
obj-y += snd-soc-aw88263.o
