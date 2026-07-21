upgtool:="../upgtools/upgtool"
scsitool:="../scsitools/scsitool"

.SUFFIXES: # disable old suffix rules

all:
	@echo "Please select an action:"
	@echo "- dump_rootfs: dumps the root filesystem to rootfs.tgz"
	@echo "- do_fw_upgrade: put the device in NWZ_DEV in firmware upgrade mode"
	@echo "- copy_fw_upgrade: copy firmware to a device with the right name"
	@echo "- exec_file: craft an upgrade that embeds and execute a file"
	@echo "- exec_file_extern: craft an upgrade that execute a file on the user partition"
	@echo "- list_targets: produce of list of available targets"
	@echo "- install_dualboot"
	@echo "- recon_hagoromo: craft an upgrade that dumps system info (Hagoromo platforms)"
	@echo "- install_dualboot_hagoromo: install dualboot on Hagoromo platforms (NW-A30...)"

ifndef UPG
want_upg:
	$(info Please set UPG to the upg filename. For example:)
	$(info make exec_file UPG=hello_world.upg ...)
	$(error "")
else
want_upg: ;
endif

ifndef EXEC
want_exec:
	$(info Please set EXEC to the executable filename. For example:)
	$(info make exec_file EXEC=hello_world.sh ...)
	$(error "")
else
want_exec: ;
endif

ifndef NWZ_TARGET
want_target:
	$(info Please set NWZ_TARGET to your target. For example:)
	$(info make dump_rootfs NWZ_TARGET=nwz-e460)
	$(info Run 'make list_targets' to get a list of all targets)
	$(error "")
else
want_target: ;
endif

ifndef NWZ_DEV
want_dev:
	$(info Please set NWZ_DEV to your dev. For example:)
	$(info make do_fw_upgrade NWZ_DEV=/dev/sdx)
	$(error "")
else
want_dev: ;
endif

ifndef NWZ_MOUNT
want_mount:
	$(info Please set NWZ_MOUNT to your dev mount point. For example:)
	$(info make copy_fw_upgrade NWZ_MOUNT=/media/WALKMAN ...)
	$(error "")
else
want_mount: ;
endif

ifndef LOG
want_log:
	$(info Please set LOG to the log filename. For example:)
	$(info make exec_file_extern LOG=exec.log ...)
	$(error "")
else
want_log: ;
endif

UPGPACK=$(upgtool) -c -m $(NWZ_TARGET) $(UPG) $(1)

exec_file: want_target want_exec want_upg
	$(call UPGPACK, exec_file.sh $(EXEC))

install_dualboot: want_target want_exec want_upg
	$(call UPGPACK, install_dualboot.sh $(EXEC))

# On Hagoromo (NW-A30+) the stock updater always extracts file 1 as md5.txt and
# aborts if it is missing, so every UPG must carry a (dummy) file 1. The dualboot
# payload is therefore file 2.
#
# EXEC must be the bootloader ELF (build/nwa30-boot/bootloader.elf), NOT
# bootloader-nwa30.sony: on Hagoromo the payload replaces HgrmMediaPlayerApp and
# is exec'd directly, whereas the .sony file is a scrambled tar for the older
# icx players' installer. Installing the .sony file gives an app that fails with
# ENOEXEC before it can log anything - the player just sits on the boot animation
# and reboots, with no clue as to why.
install_dualboot_hagoromo: want_target want_exec want_upg
	$(call UPGPACK, install_dualboot_hagoromo.sh hagoromo_md5.txt $(EXEC))

recon_hagoromo: want_target want_upg
	$(call UPGPACK, recon_hagoromo.sh hagoromo_md5.txt)

# recovery: put the stock application back if the player no longer boots
uninstall_dualboot_hagoromo: want_target want_upg
	$(call UPGPACK, uninstall_dualboot_hagoromo.sh hagoromo_md5.txt)

exec_file_extern.tmp: want_exec want_upg want_log
	cat exec_file_extern.sh.in | sed "s|NWZ_EXEC_THIS|$(EXEC)|" |\
	sed "s|NWZ_LOG_THIS|$(LOG)|" > $@
.INTERMEDIATE: exec_file_extern.tmp

exec_file_extern: want_target want_exec want_upg want_log exec_file_extern.tmp
	$(call UPGPACK, exec_file_extern.tmp)

dump_rootfs: want_target want_upg
	$(call UPGPACK, dump_rootfs.sh)

clean:
	rm -rf *.upg

list_targets:
	$(upgtool) -m ?; true # upgtool returns an error in this case, ignore it

copy_fw_upgrade: want_upg want_mount
	cp $(UPG) "$(NWZ_MOUNT)/NW_WM_FW.UPG"

do_fw_upgrade: want_dev
	@echo "Device: $(NWZ_DEV)"
	$(scsitool) $(NWZ_DEV) do_fw_upgrade
