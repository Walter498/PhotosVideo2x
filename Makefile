TARGET = iphone:clang:16.5:14.0
ARCHS = arm64 arm64e
THEOS_PACKAGE_SCHEME = rootless
INSTALL_TARGET_PROCESSES = MobileSlideShow

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = PhotosVideo2x
PhotosVideo2x_FILES = Tweak.xm
PhotosVideo2x_FRAMEWORKS = UIKit Photos AVFoundation CoreMedia
PhotosVideo2x_CFLAGS = -fobjc-arc
PhotosVideo2x_LOGOS_DEFAULT_GENERATOR = internal

include $(THEOS_MAKE_PATH)/tweak.mk
