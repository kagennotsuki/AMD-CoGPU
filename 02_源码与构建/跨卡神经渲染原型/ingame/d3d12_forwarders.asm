; d3d12 ???????? D3D12CreateDevice ??????
OPTION CASEMAP:NONE

EXTERN d3d12FwdTargets:QWORD

.CODE
PUBLIC d3dfwd_D3D12CoreCreateLayeredDevice
d3dfwd_D3D12CoreCreateLayeredDevice PROC
    jmp QWORD PTR [d3d12FwdTargets + 0]
d3dfwd_D3D12CoreCreateLayeredDevice ENDP

PUBLIC d3dfwd_D3D12CoreGetLayeredDeviceSize
d3dfwd_D3D12CoreGetLayeredDeviceSize PROC
    jmp QWORD PTR [d3d12FwdTargets + 8]
d3dfwd_D3D12CoreGetLayeredDeviceSize ENDP

PUBLIC d3dfwd_D3D12CoreRegisterLayers
d3dfwd_D3D12CoreRegisterLayers PROC
    jmp QWORD PTR [d3d12FwdTargets + 16]
d3dfwd_D3D12CoreRegisterLayers ENDP

PUBLIC d3dfwd_D3D12CreateRootSignatureDeserializer
d3dfwd_D3D12CreateRootSignatureDeserializer PROC
    jmp QWORD PTR [d3d12FwdTargets + 24]
d3dfwd_D3D12CreateRootSignatureDeserializer ENDP

PUBLIC d3dfwd_D3D12CreateVersionedRootSignatureDeserializer
d3dfwd_D3D12CreateVersionedRootSignatureDeserializer PROC
    jmp QWORD PTR [d3d12FwdTargets + 32]
d3dfwd_D3D12CreateVersionedRootSignatureDeserializer ENDP

PUBLIC d3dfwd_D3D12DeviceRemovedExtendedData
d3dfwd_D3D12DeviceRemovedExtendedData PROC
    jmp QWORD PTR [d3d12FwdTargets + 40]
d3dfwd_D3D12DeviceRemovedExtendedData ENDP

PUBLIC d3dfwd_D3D12EnableExperimentalFeatures
d3dfwd_D3D12EnableExperimentalFeatures PROC
    jmp QWORD PTR [d3d12FwdTargets + 48]
d3dfwd_D3D12EnableExperimentalFeatures ENDP

PUBLIC d3dfwd_D3D12GetDebugInterface
d3dfwd_D3D12GetDebugInterface PROC
    jmp QWORD PTR [d3d12FwdTargets + 56]
d3dfwd_D3D12GetDebugInterface ENDP

PUBLIC d3dfwd_D3D12GetInterface
d3dfwd_D3D12GetInterface PROC
    jmp QWORD PTR [d3d12FwdTargets + 64]
d3dfwd_D3D12GetInterface ENDP

PUBLIC d3dfwd_D3D12PIXEventsReplaceBlock
d3dfwd_D3D12PIXEventsReplaceBlock PROC
    jmp QWORD PTR [d3d12FwdTargets + 72]
d3dfwd_D3D12PIXEventsReplaceBlock ENDP

PUBLIC d3dfwd_D3D12PIXGetThreadInfo
d3dfwd_D3D12PIXGetThreadInfo PROC
    jmp QWORD PTR [d3d12FwdTargets + 80]
d3dfwd_D3D12PIXGetThreadInfo ENDP

PUBLIC d3dfwd_D3D12PIXNotifyWakeFromFenceSignal
d3dfwd_D3D12PIXNotifyWakeFromFenceSignal PROC
    jmp QWORD PTR [d3d12FwdTargets + 88]
d3dfwd_D3D12PIXNotifyWakeFromFenceSignal ENDP

PUBLIC d3dfwd_D3D12PIXReportCounter
d3dfwd_D3D12PIXReportCounter PROC
    jmp QWORD PTR [d3d12FwdTargets + 96]
d3dfwd_D3D12PIXReportCounter ENDP

PUBLIC d3dfwd_D3D12SerializeRootSignature
d3dfwd_D3D12SerializeRootSignature PROC
    jmp QWORD PTR [d3d12FwdTargets + 104]
d3dfwd_D3D12SerializeRootSignature ENDP

PUBLIC d3dfwd_D3D12SerializeVersionedRootSignature
d3dfwd_D3D12SerializeVersionedRootSignature PROC
    jmp QWORD PTR [d3d12FwdTargets + 112]
d3dfwd_D3D12SerializeVersionedRootSignature ENDP

PUBLIC d3dfwd_GetBehaviorValue
d3dfwd_GetBehaviorValue PROC
    jmp QWORD PTR [d3d12FwdTargets + 120]
d3dfwd_GetBehaviorValue ENDP

PUBLIC d3dfwd_SetAppCompatStringPointer
d3dfwd_SetAppCompatStringPointer PROC
    jmp QWORD PTR [d3d12FwdTargets + 128]
d3dfwd_SetAppCompatStringPointer ENDP

END
