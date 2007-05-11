BASEDIR=$(shell pwd)

MAJOR=1
MINOR=2
SUBMINOR=0

INSTALL_PREFIX := /
export INSTALL_PREFIX

#PATH to linux source/headers
#LINUX=/usr/src/linux

ifndef KVERS
KVERS:=$(shell uname -r)
endif

MODS=/lib/modules/$(KVERS)
LINUX=$(MODS)/build
LINUX_SOURCE=$(MODS)/source
UPDATE_MODULES=$(shell which update-modules)
MODULES_UPDATE=$(shell which modules-update)
DEPMOD=$(shell which depmod)


MISDNDIR=$(BASEDIR)
MISDN_SRC=$(MISDNDIR)/drivers/isdn/hardware/mISDN

########################################
# USER CONFIGS END
########################################

CONFIGS+=CONFIG_MISDN_DRV=m 
CONFIGS+=CONFIG_MISDN_DSP=m 
CONFIGS+=CONFIG_MISDN_HFCMULTI=m 
CONFIGS+=CONFIG_MISDN_HFCPCI=m
CONFIGS+=CONFIG_MISDN_HFCUSB=m
CONFIGS+=CONFIG_MISDN_XHFC=m
CONFIGS+=CONFIG_MISDN_HFCMINI=m
CONFIGS+=CONFIG_MISDN_W6692=m
CONFIGS+=CONFIG_MISDN_SPEEDFAX=m
CONFIGS+=CONFIG_MISDN_AVM_FRITZ=m
CONFIGS+=CONFIG_MISDN_NETJET=m
CONFIGS+=CONFIG_MISDN_L1OIP=m 

#CONFIGS+=CONFIG_MISDN_NETDEV=y

MISDNVERSION=$(shell cat VERSION)

MINCLUDES+=-I$(MISDNDIR)/include

all: VERSION test_old_misdn
	cp $(MISDNDIR)/drivers/isdn/hardware/mISDN/Makefile.v2.6 $(MISDNDIR)/drivers/isdn/hardware/mISDN/Makefile
	export MINCLUDES=$(MISDNDIR)/include ; export MISDNVERSION=$(MISDNVERSION); make -C $(LINUX) SUBDIRS=$(MISDN_SRC) modules $(CONFIGS)  

install: all modules-install
	$(DEPMOD) 
	$(UPDATE_MODULES)
	$(MODULES_UPDATE)
	make -C config install

modules-install:
	cd $(LINUX) ; make INSTALL_MOD_PATH=$(INSTALL_PREFIX) SUBDIRS=$(MISDN_SRC) modules_install 
	mkdir -p $(INSTALL_PREFIX)/usr/include/linux/
	cp $(MISDNDIR)/include/linux/*.h $(INSTALL_PREFIX)/usr/include/linux/

test_old_misdn:
	@if echo -ne "#include <linux/mISDNif.h>" | gcc -C -E - 2>/dev/null 1>/dev/null  ; then \
		if ! echo -ne "#include <linux/mISDNif.h>\n#if MISDN_MAJOR_VERSION < 4\n#error old mISDNif.h\n#endif\n" | gcc -C -E - 2>/dev/null 1>/dev/null ; then \
			echo -ne "\n!!You should remove the following files:\n\n$(LINUX)/include/linux/mISDNif.h\n$(LINUX)/include/linux/isdn_compat.h\n/usr/include/linux/mISDNif.h\n/usr/include/linux/isdn_compat.h\n\nIn order to upgrade to the mqueue branch\n\n"; \
			echo -ne "I can do that for you, just type: make force\n\n" ; \
			exit 1; \
		fi ;\
	fi



.PHONY: modules-install install all clean VERSION

force:
	rm -f $(LINUX)/include/linux/mISDNif.h
	rm -f $(LINUX)/include/linux/isdn_compat.h
	rm -f /usr/include/linux/mISDNif.h
	rm -f /usr/include/linux/isdn_compat.h

clean:
	rm -rf drivers/isdn/hardware/mISDN/*.o
	rm -rf drivers/isdn/hardware/mISDN/*.ko
	rm -rf *~
	find . -iname ".*.cmd" -exec rm -rf {} \;
	find . -iname ".*.d" -exec rm -rf {} \;
	find . -iname "*.mod.c" -exec rm -rf {} \;
	find . -iname "*.mod" -exec rm -rf {} \;

VERSION:
	echo $(MAJOR)_$(MINOR)_$(SUBMINOR) > VERSION ; \

snapshot: clean
	DIR=mISDN-$$(date +"20%y_%m_%d") ; \
	echo $(MAJOR)_$(MINOR)_$(SUBMINOR)-$$(date +"20%y_%m_%d" | sed -e "s/\//_/g") > VERSION ; \
	mkdir -p /tmp/$$DIR ; \
	cp -a * /tmp/$$DIR ; \
	cd /tmp/; \
	tar czf $$DIR.tar.gz $$DIR

release: clean
	DIR=mISDN-$(MAJOR)_$(MINOR)_$(SUBMINOR) ; \
	echo $(MAJOR)_$(MINOR)_$(SUBMINOR) > VERSION ; \
	mkdir -p /tmp/$$DIR ; \
	cp -a * /tmp/$$DIR ; \
	cd /tmp/; \
	tar czf $$DIR.tar.gz $$DIR

%-package:
	git-archive --format=tar $* | gzip > $*-$$(date +"20%y_%m_%d").tar.gz
