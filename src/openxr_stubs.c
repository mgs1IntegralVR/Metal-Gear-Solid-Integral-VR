#include <windows.h>
#include <openxr/openxr.h>

static HMODULE g_openxrLoader = NULL;
static int g_loaderInitTried = 0;

static void EnsureOpenXRLoaderLoaded() {
	if (g_loaderInitTried) {
		return;
	}

	g_loaderInitTried = 1;
	g_openxrLoader = LoadLibraryA("openxr_loader.dll");

	if (!g_openxrLoader) {
		OutputDebugStringA("[VR] OpenXR loader not found (openxr_loader.dll). Running in fallback mode.\n");
	}
	else {
		OutputDebugStringA("[VR] OpenXR loader loaded successfully.\n");
	}
}

#define XR_FORWARD_OR_ERROR(signature, name, args) \
XRAPI_ATTR XrResult XRAPI_CALL name signature { \
	EnsureOpenXRLoaderLoaded(); \
	if (!g_openxrLoader) { \
		return XR_ERROR_RUNTIME_UNAVAILABLE; \
	} \
	typedef XRAPI_ATTR XrResult(XRAPI_CALL* FnType) signature; \
	static FnType fn = NULL; \
	if (!fn) { \
		fn = (FnType)GetProcAddress(g_openxrLoader, #name); \
	} \
	if (!fn) { \
		OutputDebugStringA("[VR] Missing OpenXR symbol: " #name "\n"); \
		return XR_ERROR_FUNCTION_UNSUPPORTED; \
	} \
	return fn args; \
}

XR_FORWARD_OR_ERROR((const XrInstanceCreateInfo* createInfo, XrInstance* instance), xrCreateInstance, (createInfo, instance))
XR_FORWARD_OR_ERROR((XrSession session, const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState), xrWaitFrame, (session, frameWaitInfo, frameState))
XR_FORWARD_OR_ERROR((XrSession session, const XrFrameBeginInfo* frameBeginInfo), xrBeginFrame, (session, frameBeginInfo))
XR_FORWARD_OR_ERROR((XrSession session, const XrFrameEndInfo* frameEndInfo), xrEndFrame, (session, frameEndInfo))
XR_FORWARD_OR_ERROR((XrSession session, const XrViewLocateInfo* viewLocateInfo, XrViewState* viewState, uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrView* views), xrLocateViews, (session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views))
XR_FORWARD_OR_ERROR((XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType, uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrViewConfigurationView* views), xrEnumerateViewConfigurationViews, (instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views))
XR_FORWARD_OR_ERROR((XrSession session, uint32_t formatCapacityInput, uint32_t* formatCountOutput, int64_t* formats), xrEnumerateSwapchainFormats, (session, formatCapacityInput, formatCountOutput, formats))
XR_FORWARD_OR_ERROR((XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain), xrCreateSwapchain, (session, createInfo, swapchain))
XR_FORWARD_OR_ERROR((XrSwapchain swapchain), xrDestroySwapchain, (swapchain))
XR_FORWARD_OR_ERROR((XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images), xrEnumerateSwapchainImages, (swapchain, imageCapacityInput, imageCountOutput, images))
XR_FORWARD_OR_ERROR((XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index), xrAcquireSwapchainImage, (swapchain, acquireInfo, index))
XR_FORWARD_OR_ERROR((XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo), xrWaitSwapchainImage, (swapchain, waitInfo))
XR_FORWARD_OR_ERROR((XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo), xrReleaseSwapchainImage, (swapchain, releaseInfo))
XR_FORWARD_OR_ERROR((XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location), xrLocateSpace, (space, baseSpace, time, location))
XR_FORWARD_OR_ERROR((XrInstance instance, const char* pathString, XrPath* path), xrStringToPath, (instance, pathString, path))
XR_FORWARD_OR_ERROR((XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet), xrCreateActionSet, (instance, createInfo, actionSet))
XR_FORWARD_OR_ERROR((XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action), xrCreateAction, (actionSet, createInfo, action))
XR_FORWARD_OR_ERROR((XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings), xrSuggestInteractionProfileBindings, (instance, suggestedBindings))
XR_FORWARD_OR_ERROR((XrSession session, const XrSessionActionSetsAttachInfo* attachInfo), xrAttachSessionActionSets, (session, attachInfo))
XR_FORWARD_OR_ERROR((XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space), xrCreateActionSpace, (session, createInfo, space))
XR_FORWARD_OR_ERROR((XrSession session, const XrActionsSyncInfo* syncInfo), xrSyncActions, (session, syncInfo))
XR_FORWARD_OR_ERROR((XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateVector2f* state), xrGetActionStateVector2f, (session, getInfo, state))
XR_FORWARD_OR_ERROR((XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateBoolean* state), xrGetActionStateBoolean, (session, getInfo, state))
XR_FORWARD_OR_ERROR((XrSession session, const XrHapticActionInfo* hapticActionInfo, const XrHapticBaseHeader* hapticFeedback), xrApplyHapticFeedback, (session, hapticActionInfo, hapticFeedback))
XR_FORWARD_OR_ERROR((XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId), xrGetSystem, (instance, getInfo, systemId))
XR_FORWARD_OR_ERROR((XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session), xrCreateSession, (instance, createInfo, session))
XR_FORWARD_OR_ERROR((XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space), xrCreateReferenceSpace, (session, createInfo, space))
XR_FORWARD_OR_ERROR((XrSession session, const XrSessionBeginInfo* beginInfo), xrBeginSession, (session, beginInfo))
XR_FORWARD_OR_ERROR((XrSession session), xrEndSession, (session))
XR_FORWARD_OR_ERROR((XrInstance instance, XrEventDataBuffer* eventData), xrPollEvent, (instance, eventData))
XR_FORWARD_OR_ERROR((const char* layerName, uint32_t propertyCapacityInput, uint32_t* propertyCountOutput, XrExtensionProperties* properties), xrEnumerateInstanceExtensionProperties, (layerName, propertyCapacityInput, propertyCountOutput, properties))
XR_FORWARD_OR_ERROR((XrInstance instance, const char* name, PFN_xrVoidFunction* function), xrGetInstanceProcAddr, (instance, name, function))
XR_FORWARD_OR_ERROR((XrSpace space), xrDestroySpace, (space))
XR_FORWARD_OR_ERROR((XrSession session), xrDestroySession, (session))
XR_FORWARD_OR_ERROR((XrInstance instance), xrDestroyInstance, (instance))
