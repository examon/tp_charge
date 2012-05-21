obj-m += tp_charge.o

.PHONY: modules
modules:
	make -C /lib/modules/`uname -r`/build SUBDIRS=`/bin/pwd`
