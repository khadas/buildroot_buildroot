################################################################################
#
# amlogic tvin driver
#
################################################################################

AML_TVIN_VERSION = $(call qstrip,$(BR2_PACKAGE_AML_TVIN_GIT_VERSION))
AML_TVIN_SITE = $(call qstrip,$(BR2_PACKAGE_AML_TVIN_GIT_REPO_URL))

ifeq ($(BR2_PACKAGE_AML_TVIN_LOCAL),y)
AML_TVIN_SITE = $(call qstrip,$(BR2_PACKAGE_AML_TVIN_LOCAL_PATH))
AML_TVIN_SITE_METHOD = local
endif
define AML_TVIN_BUILD_CMDS
	mkdir -p $(LINUX_DIR)/../hardware;
	ln -sf $(AML_TVIN_DIR) $(LINUX_DIR)/../hardware/tvin
endef
$(eval $(generic-package))
