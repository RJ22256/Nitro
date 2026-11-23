; x64 export thunks: each exported dxgi function jumps straight to the real
; System32\dxgi.dll implementation (addresses filled in by DllMain).
; Indices must match kDxgiExports in dllmain.cpp.

.data
PUBLIC g_dxgiProcs
g_dxgiProcs QWORD 32 DUP(0)

.code

THUNK MACRO name, index
name PROC
    jmp QWORD PTR [g_dxgiProcs + index * 8]
name ENDP
ENDM

THUNK Thunk_ApplyCompatResolutionQuirking, 0
THUNK Thunk_CompatString, 1
THUNK Thunk_CompatValue, 2
THUNK Thunk_CreateDXGIFactory, 3
THUNK Thunk_CreateDXGIFactory1, 4
THUNK Thunk_CreateDXGIFactory2, 5
THUNK Thunk_DXGID3D10CreateDevice, 6
THUNK Thunk_DXGID3D10CreateLayeredDevice, 7
THUNK Thunk_DXGID3D10GetLayeredDeviceSize, 8
THUNK Thunk_DXGID3D10RegisterLayers, 9
THUNK Thunk_DXGIDeclareAdapterRemovalSupport, 10
THUNK Thunk_DXGIDisableVBlankVirtualization, 11
THUNK Thunk_DXGIDumpJournal, 12
THUNK Thunk_DXGIGetDebugInterface1, 13
THUNK Thunk_DXGIReportAdapterConfiguration, 14
THUNK Thunk_PIXBeginCapture, 15
THUNK Thunk_PIXEndCapture, 16
THUNK Thunk_PIXGetCaptureState, 17
THUNK Thunk_SetAppCompatStringPointer, 18
THUNK Thunk_UpdateHMDEmulationStatus, 19
THUNK Thunk_DXGIRevertToSxS, 20

END
