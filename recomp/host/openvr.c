// OpenVR: there is no VR runtime here, and the game is told so.
#include "host.h"

IMPORT(VR_IsRuntimeInstalled) { RET(0); }
IMPORT(VR_IsHmdPresent) { RET(0); }
IMPORT(VR_InitInternal2) { if (ARG(0)) st32(ARG(0), 100); RET(0); }   // VRInitError_Init_InstallationNotFound
IMPORT(VR_GetVRInitErrorAsEnglishDescription) { RET("No VR runtime is available on this system"); }
IMPORT(VR_IsInterfaceVersionValid) { RET(0); }
IMPORT(VR_GetInitToken) { RET(0); }
IMPORT(VR_GetGenericInterface) { if (ARG(1)) st32(ARG(1), 100); RET(0); }
IMPORT(VR_ShutdownInternal) { (void)c; }
