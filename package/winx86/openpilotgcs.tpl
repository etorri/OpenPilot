#
# *****************************************************************************
#
# @file       ${OUTFILENAME}
# @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2011.
# @brief      Autogenerated NSIS header file, built using template
#             ${TEMPLATE}
#
# @see        The GNU Public License (GPL) Version 3
#
# *****************************************************************************
#

; Some names, paths and constants
!define PACKAGE_LBL "${PACKAGE_LBL}"
!define PACKAGE_DIR "..\..\build\package-$${PACKAGE_LBL}"
!define OUT_FILE "OpenPilot-$${PACKAGE_LBL}-install.exe"
!define FIRMWARE_DIR "firmware-$${PACKAGE_LBL}"

; Installer version info
!define PRODUCT_VERSION "0.0.0.0"
!define FILE_VERSION "${TAG_OR_BRANCH}:${HASH8}${DIRTY} ${DATETIME}"
!define BUILD_DESCRIPTION "${PACKAGE_LBL} built using ${ORIGIN} as origin, committed ${DATETIME} as ${HASH}"
