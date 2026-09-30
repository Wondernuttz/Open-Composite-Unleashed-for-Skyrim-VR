#pragma once
#include "generated/interfaces/openvr.h"
#include "generated/interfaces/vrtypes.h"
#include "generated/interfaces/vrannotation.h"

namespace vr
{
namespace IVRSystem_023
{

#if defined(__linux__) || defined(__APPLE__)
#pragma pack(push, 4)
#else
#pragma pack(push, 8)
#endif

class IVRSystem
{
public:
	virtual void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) = 0;
	virtual HmdMatrix44_t GetProjectionMatrix(EVREye eEye, float fNearZ, float fFarZ) = 0;
	virtual void GetProjectionRaw(EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) = 0;
	virtual bool ComputeDistortion(EVREye eEye, float fU, float fV, DistortionCoordinates_t* pDistortionCoordinates) = 0;
	virtual HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye) = 0;
	virtual bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) = 0;
	virtual int32_t GetD3D9AdapterIndex() = 0;
	virtual void GetDXGIOutputInfo(int32_t* pnAdapterIndex) = 0;
	virtual void GetOutputDevice(uint64_t* pnDevice, ETextureType textureType, VkInstance_T* pInstance = nullptr) = 0;
	virtual bool IsDisplayOnDesktop() = 0;
	virtual bool SetDisplayVisibility(bool bIsVisibleOnDesktop) = 0;
	virtual void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, VR_ARRAY_COUNT(unTrackedDevicePoseArrayCount) TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) = 0;
	virtual HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() = 0;
	virtual HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() = 0;
	virtual uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass eTrackedDeviceClass, VR_ARRAY_COUNT(unTrackedDeviceIndexArrayCount) vr::TrackedDeviceIndex_t* punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex = k_unTrackedDeviceIndex_Hmd) = 0;
	virtual EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) = 0;
	virtual void ApplyTransform(TrackedDevicePose_t* pOutputPose, const TrackedDevicePose_t* pTrackedDevicePose, const HmdMatrix34_t* pTransform) = 0;
	virtual TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole unDeviceType) = 0;
	virtual ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(TrackedDeviceIndex_t unDeviceIndex) = 0;
	virtual ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) = 0;
	virtual bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) = 0;
	virtual bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError = 0L) = 0;
	virtual float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError = 0L) = 0;
	virtual int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError = 0L) = 0;
	virtual uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError = 0L) = 0;
	virtual HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pError = 0L) = 0;
	virtual uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, PropertyTypeTag_t propType, void* pBuffer, uint32_t unBufferSize, ETrackedPropertyError* pError = 0L) = 0;
	virtual uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, VR_OUT_STRING() char* pchValue, uint32_t unBufferSize, ETrackedPropertyError* pError = 0L) = 0;
	virtual const char *GetPropErrorNameFromEnum(ETrackedPropertyError error) = 0;
	virtual bool PollNextEvent(VREvent_t* pEvent, uint32_t uncbVREvent) = 0;
	virtual bool PollNextEventWithPose(ETrackingUniverseOrigin eOrigin, VREvent_t* pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t* pTrackedDevicePose) = 0;
	virtual bool PollNextEventWithPoseAndOverlays(vr::ETrackingUniverseOrigin eOrigin, VREvent_t* pEvent, uint32_t uncbVREvent, TrackedDevicePose_t* pTrackedDevicePose, vr::VROverlayHandle_t* pulOverlayHandle) = 0;
	virtual const char *GetEventTypeNameFromEnum(EVREventType eType) = 0;
	virtual HiddenAreaMesh_t GetHiddenAreaMesh(EVREye eEye, EHiddenAreaMeshType type = k_eHiddenAreaMesh_Standard) = 0;
	virtual bool GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t* pControllerState, uint32_t unControllerStateSize) = 0;
	virtual bool GetControllerStateWithPose(ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t* pControllerState, uint32_t unControllerStateSize, TrackedDevicePose_t* pTrackedDevicePose) = 0;
	virtual void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) = 0;
	virtual const char *GetButtonIdNameFromEnum(EVRButtonId eButtonId) = 0;
	virtual const char *GetControllerAxisTypeNameFromEnum(EVRControllerAxisType eAxisType) = 0;
	virtual bool IsInputAvailable() = 0;
	virtual bool IsSteamVRDrawingControllers() = 0;
	virtual bool ShouldApplicationPause() = 0;
	virtual bool ShouldApplicationReduceRenderingWork() = 0;
	virtual EVRFirmwareError PerformFirmwareUpdate(TrackedDeviceIndex_t unDeviceIndex) = 0;
	virtual void AcknowledgeQuit_Exiting() = 0;
	virtual uint32_t GetAppContainerFilePaths(VR_OUT_STRING() char* pchBuffer, uint32_t unBufferSize) = 0;
	virtual const char *GetRuntimeVersion() = 0;
};

static const char* const IVRSystem_Version = "IVRSystem_023";

#pragma pack(pop)

} // namespace IVRSystem_023
} // namespace vr
