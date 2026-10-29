; dxgi ??????? ?? ??????????? dxgi_orig.dll
; ? build ?????????
OPTION CASEMAP:NONE

EXTERN fwdTargets:QWORD

.CODE
PUBLIC fwd_ApplyCompatResolutionQuirking
fwd_ApplyCompatResolutionQuirking PROC
    jmp QWORD PTR [fwdTargets + 0]
fwd_ApplyCompatResolutionQuirking ENDP

PUBLIC fwd_CompatString
fwd_CompatString PROC
    jmp QWORD PTR [fwdTargets + 8]
fwd_CompatString ENDP

PUBLIC fwd_CompatValue
fwd_CompatValue PROC
    jmp QWORD PTR [fwdTargets + 16]
fwd_CompatValue ENDP

PUBLIC fwd_CreateDXGIFactory
fwd_CreateDXGIFactory PROC
    jmp QWORD PTR [fwdTargets + 24]
fwd_CreateDXGIFactory ENDP

PUBLIC fwd_DXGID3D10CreateDevice
fwd_DXGID3D10CreateDevice PROC
    jmp QWORD PTR [fwdTargets + 32]
fwd_DXGID3D10CreateDevice ENDP

PUBLIC fwd_DXGID3D10CreateLayeredDevice
fwd_DXGID3D10CreateLayeredDevice PROC
    jmp QWORD PTR [fwdTargets + 40]
fwd_DXGID3D10CreateLayeredDevice ENDP

PUBLIC fwd_DXGID3D10GetLayeredDeviceSize
fwd_DXGID3D10GetLayeredDeviceSize PROC
    jmp QWORD PTR [fwdTargets + 48]
fwd_DXGID3D10GetLayeredDeviceSize ENDP

PUBLIC fwd_DXGID3D10RegisterLayers
fwd_DXGID3D10RegisterLayers PROC
    jmp QWORD PTR [fwdTargets + 56]
fwd_DXGID3D10RegisterLayers ENDP

PUBLIC fwd_DXGIDeclareAdapterRemovalSupport
fwd_DXGIDeclareAdapterRemovalSupport PROC
    jmp QWORD PTR [fwdTargets + 64]
fwd_DXGIDeclareAdapterRemovalSupport ENDP

PUBLIC fwd_DXGIDisableVBlankVirtualization
fwd_DXGIDisableVBlankVirtualization PROC
    jmp QWORD PTR [fwdTargets + 72]
fwd_DXGIDisableVBlankVirtualization ENDP

PUBLIC fwd_DXGIDumpJournal
fwd_DXGIDumpJournal PROC
    jmp QWORD PTR [fwdTargets + 80]
fwd_DXGIDumpJournal ENDP

PUBLIC fwd_DXGIGetDebugInterface1
fwd_DXGIGetDebugInterface1 PROC
    jmp QWORD PTR [fwdTargets + 88]
fwd_DXGIGetDebugInterface1 ENDP

PUBLIC fwd_DXGIReportAdapterConfiguration
fwd_DXGIReportAdapterConfiguration PROC
    jmp QWORD PTR [fwdTargets + 96]
fwd_DXGIReportAdapterConfiguration ENDP

PUBLIC fwd_PIXBeginCapture
fwd_PIXBeginCapture PROC
    jmp QWORD PTR [fwdTargets + 104]
fwd_PIXBeginCapture ENDP

PUBLIC fwd_PIXEndCapture
fwd_PIXEndCapture PROC
    jmp QWORD PTR [fwdTargets + 112]
fwd_PIXEndCapture ENDP

PUBLIC fwd_PIXGetCaptureState
fwd_PIXGetCaptureState PROC
    jmp QWORD PTR [fwdTargets + 120]
fwd_PIXGetCaptureState ENDP

PUBLIC fwd_SetAppCompatStringPointer
fwd_SetAppCompatStringPointer PROC
    jmp QWORD PTR [fwdTargets + 128]
fwd_SetAppCompatStringPointer ENDP

PUBLIC fwd_UpdateHMDEmulationStatus
fwd_UpdateHMDEmulationStatus PROC
    jmp QWORD PTR [fwdTargets + 136]
fwd_UpdateHMDEmulationStatus ENDP

END
