#include <initguid.h>
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>
#include <usbdlib.h>
#include <udecx.h>
#include <ntstrsafe.h>
#include <usbioctl.h>
#include <Usbiodef.h>
#include <usbbusif.h>

/* This example supports these scenarios:
 * 1. The device is removed immediately after being plugged in.
 *	UDECXUSBDEVICE and all attached contexts remain until the driver is unloaded
 * 2. The device is removed after the first control request.
 *	UDECXUSBDEVICE gets destroyed properly, but some udecx-internal allocations remain.
 *	Most notably, a WDFQUEUE with an udecx!CONTROL_QUEUE_CONTEXT, created by
 *	UsbDevice_UcxUsbDeviceCreate
 * 
 * The test can involve multiple devices to amplify the effect.
 */

#define SCENARIO 1
#define NUM_DEVICES 4

#define Debug(...) DbgPrintEx(DPFLTR_IHVBUS_ID, DPFLTR_INFO_LEVEL, __VA_ARGS__) 

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD                 Controller_WdfEvtDeviceAdd;

#define BASE_DEVICE_NAME                  L"\\Device\\USBFDO-"
#define BASE_SYMBOLIC_LINK_NAME           L"\\DosDevices\\HCD"

#define MAX_SUFFIX_SIZE (16)

#define DeviceNameSize                    sizeof(BASE_DEVICE_NAME)+MAX_SUFFIX_SIZE
#define SymLinkNameSize                   sizeof(BASE_SYMBOLIC_LINK_NAME)+MAX_SUFFIX_SIZE
#define USB_HOST_DEVINTERFACE_REF_STRING L"GUID_DEVINTERFACE_USB_HOST_CONTROLLER"

struct USBDEVICE_CONTEXT {
	WDFDPC dpc;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USBDEVICE_CONTEXT, UsbDeviceGetContext)

struct USBQUEUE_CONTEXT {
	UDECXUSBDEVICE UdecxUsbDevice;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USBQUEUE_CONTEXT, UsbQueueGetContext)


EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL        Controller_EvtIoDeviceControl;

VOID Controller_EvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t , _In_ size_t , _In_ ULONG )
{
	if(!UdecxWdfDeviceTryHandleUserIoctl(WdfIoQueueGetDevice(Queue), Request))
		WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

NTSTATUS Controller_EvtControllerQueryUsbCapability(WDFDEVICE , PGUID , ULONG , PVOID , PULONG ResultLength)
{
	*ResultLength = 0;
	return STATUS_NOT_IMPLEMENTED;
}
#include <usbspec.h>

const USB_DEVICE_DESCRIPTOR g_UsbDeviceDescriptor = {
	sizeof(USB_DEVICE_DESCRIPTOR),
	USB_DEVICE_DESCRIPTOR_TYPE,
	0x0200, // usb 2.0
	0xff, 0xff, 0xff, // class, subclass, protocol - vendor specific
	8, // max control packet size
	0x1234, // vid
	0x5678, // pid
	0x0101, // version
	1, 2, 3, // manufacturer, product, serial string
	1, // number of configurations
};

struct MY_DESCRIPTORS{
	USB_CONFIGURATION_DESCRIPTOR conf;
	USB_INTERFACE_DESCRIPTOR intf0;
} g_UsbConfigDescriptorSet = {
	{
		sizeof(USB_CONFIGURATION_DESCRIPTOR),
		USB_CONFIGURATION_DESCRIPTOR_TYPE,
		sizeof(MY_DESCRIPTORS), // total length, including inlined descriptors
		1, // number of interfaces
		1, // index
		0, // cfg. name string
		USB_CONFIG_BUS_POWERED, // attributes
		1, // bus current, not relevant
	},

	{
		sizeof(USB_INTERFACE_DESCRIPTOR),
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0, // index
		0, // altsetting
		0, // num endpoints
		0xff, 0xff, 0xff, // class, subclass, protocol
		0, // if name string
	},
};

static VOID IoEvtControlUrb(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t , _In_ size_t , _In_ ULONG )
{
	(void)Queue;
#if SCENARIO == 1
	Debug("SHOULD NOT REACH!\n");
	DbgRaiseAssertionFailure();
#endif

#if SCENARIO == 2
	Debug("USB control request received!\n");
	auto qc = UsbQueueGetContext(Queue);
	if(qc->UdecxUsbDevice) {
		WdfDpcEnqueue(UsbDeviceGetContext(qc->UdecxUsbDevice)->dpc);
		qc->UdecxUsbDevice = 0;
	}
#endif

	return WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

VOID UsbEndpointReset(_In_ UDECXUSBENDPOINT , _In_ WDFREQUEST Request)
{
	WdfRequestComplete(Request, STATUS_SUCCESS);
}

void UnplugDevice(UDECXUSBDEVICE Device)
{
	NTSTATUS status = UdecxUsbDevicePlugOutAndDelete(Device);
	if(!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		// is this the right thing to do?
		WdfObjectDelete(Device);
		Debug("USB device deleted forcefully\n");
	} else {
		Debug("USB device plugged out\n");
	}
}


void EvtDpcFunc(WDFDPC dpc)
{
	UnplugDevice((UDECXUSBDEVICE)WdfDpcGetParentObject(dpc));
}

NTSTATUS Usb_Create(_In_ WDFDEVICE WdfDevice, UDECXUSBDEVICE & UdecxUsbDevice)
{
	auto UdecxUsbDeviceInit = UdecxUsbDeviceInitAllocate(WdfDevice);

	if (UdecxUsbDeviceInit == NULL) {
		return STATUS_UNEXPECTED_IO_ERROR;
	}

	// State changed callbacks

	UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS udeCallbacks;
	UDECX_USB_DEVICE_CALLBACKS_INIT(&udeCallbacks);
	UdecxUsbDeviceInitSetStateChangeCallbacks(UdecxUsbDeviceInit, &udeCallbacks);

	// Set required attributes.
	UdecxUsbDeviceInitSetSpeed(UdecxUsbDeviceInit, UdecxUsbFullSpeed);
	UdecxUsbDeviceInitSetEndpointsType(UdecxUsbDeviceInit, UdecxEndpointTypeSimple);

	// Add device descriptor
	//
	auto status = UdecxUsbDeviceInitAddDescriptor(UdecxUsbDeviceInit,
		(PUCHAR)&g_UsbDeviceDescriptor,
		sizeof(g_UsbDeviceDescriptor));

	if (!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	status = UdecxUsbDeviceInitAddDescriptor(UdecxUsbDeviceInit,
		(PUCHAR)&g_UsbConfigDescriptorSet,
		sizeof(g_UsbConfigDescriptorSet));

	if (!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	// Note that we don't load any string descriptors. When the OS attempts to load them,
	// UDECX will forward the control request to us.

	WDF_OBJECT_ATTRIBUTES udeAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&udeAttributes, USBDEVICE_CONTEXT);
	udeAttributes.EvtCleanupCallback = [](WDFOBJECT o) {
		Debug("UDECX device context cleanup for %p\n", o);
	};
	udeAttributes.EvtDestroyCallback = [](WDFOBJECT o) {
		Debug("UDECX device context destroy for %p\n", o);
	};

	status = UdecxUsbDeviceCreate(&UdecxUsbDeviceInit, &udeAttributes, &UdecxUsbDevice);

	if (!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	auto devContext = UsbDeviceGetContext(UdecxUsbDevice);
	Debug("UDECX device context create for %p\n", UdecxUsbDevice);


	WDF_IO_QUEUE_CONFIG queueConfig;
	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
	queueConfig.EvtIoInternalDeviceControl = IoEvtControlUrb;
	WDF_OBJECT_ATTRIBUTES queueAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queueAttributes, USBQUEUE_CONTEXT);
	WDFQUEUE controlQueue;
	status = WdfIoQueueCreate (WdfDevice, &queueConfig, &queueAttributes, &controlQueue);

	if (!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	UsbQueueGetContext(controlQueue)->UdecxUsbDevice = UdecxUsbDevice;

	PUDECXUSBENDPOINT_INIT endpointInit = UdecxUsbSimpleEndpointInitAllocate(UdecxUsbDevice);
	if (endpointInit == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	UdecxUsbEndpointInitSetEndpointAddress(endpointInit, USB_DEFAULT_ENDPOINT_ADDRESS);

	UDECX_USB_ENDPOINT_CALLBACKS epCallbacks;
	UDECX_USB_ENDPOINT_CALLBACKS_INIT(&epCallbacks, UsbEndpointReset);
	UdecxUsbEndpointInitSetCallbacks(endpointInit, &epCallbacks);

	UDECXUSBENDPOINT ep0;
	status = UdecxUsbEndpointCreate(&endpointInit, WDF_NO_OBJECT_ATTRIBUTES, &ep0);

	if (!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	UdecxUsbEndpointSetWdfIoQueue(ep0, controlQueue);

	WDF_DPC_CONFIG dpcConfig;
	WDF_DPC_CONFIG_INIT(&dpcConfig, EvtDpcFunc);
	WDF_OBJECT_ATTRIBUTES dpcAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
	dpcAttributes.ParentObject = UdecxUsbDevice;

	status = WdfDpcCreate(&dpcConfig, &dpcAttributes, &devContext->dpc );

	if (!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}


	status = UdecxUsbDevicePlugIn(UdecxUsbDevice, NULL);

	if (!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	return status;
}

NTSTATUS Controller_WdfEvtDeviceAdd(_In_ WDFDRIVER, _Inout_ PWDFDEVICE_INIT WdfDeviceInit)
{
	DECLARE_UNICODE_STRING_SIZE(uniDeviceName, DeviceNameSize);
	DECLARE_UNICODE_STRING_SIZE(uniSymLinkName, SymLinkNameSize);

	// Do additional setup required for USB controllers.
	NTSTATUS status = UdecxInitializeWdfDeviceInit(WdfDeviceInit);
	if(!NT_SUCCESS(status))
		return status;

	WDF_OBJECT_ATTRIBUTES wdfDeviceAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&wdfDeviceAttributes);

	WDFDEVICE wdfDevice;

	// Call WdfDeviceCreate with a few extra compatibility steps to ensure this device looks
	// exactly like other USB host controllers.
	for(ULONG instanceNumber = 0;; instanceNumber++) {
		status = RtlUnicodeStringPrintf(&uniDeviceName,
			L"%ws%d",
			BASE_DEVICE_NAME,
			instanceNumber);

		if(!NT_SUCCESS(status)) {
			Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
			return status;
		}

		status = WdfDeviceInitAssignName(WdfDeviceInit, &uniDeviceName);
		if(!NT_SUCCESS(status)) {
			Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
			return status;
		}

		status = WdfDeviceCreate(&WdfDeviceInit, &wdfDeviceAttributes, &wdfDevice);

		if(status == STATUS_OBJECT_NAME_COLLISION) {

			// This is expected to happen at least once when another USB host controller
			// already exists on the system.


		} else if(!NT_SUCCESS(status)) {
			Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);

		} else {
			// Create the symbolic link (also for compatibility).
			status = RtlUnicodeStringPrintf(&uniSymLinkName,
				L"%ws%d",
				BASE_SYMBOLIC_LINK_NAME,
				instanceNumber);
			break;
		}
	}

	if(!NT_SUCCESS(status))
		return status;

	status = WdfDeviceCreateSymbolicLink(wdfDevice, &uniSymLinkName);
	if(!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	// Create the device interface.
	UNICODE_STRING                      refString;
	RtlInitUnicodeString(&refString, USB_HOST_DEVINTERFACE_REF_STRING);

	status = WdfDeviceCreateDeviceInterface(wdfDevice, (LPGUID)&GUID_DEVINTERFACE_USB_HOST_CONTROLLER, &refString);
	if(!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	UDECX_WDF_DEVICE_CONFIG controllerConfig;
	UDECX_WDF_DEVICE_CONFIG_INIT(&controllerConfig, Controller_EvtControllerQueryUsbCapability);
	controllerConfig.NumberOfUsb20Ports = NUM_DEVICES;

	status = UdecxWdfDeviceAddUsbDeviceEmulation(wdfDevice, &controllerConfig);
	if(!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	// Create default queue. It only supports USB controller IOCTLs. (USB I/O will come through
	// in separate USB device queues.)
	// Shown later in this topic.

	WDF_IO_QUEUE_CONFIG                 defaultQueueConfig;
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&defaultQueueConfig, WdfIoQueueDispatchSequential);
	defaultQueueConfig.EvtIoDeviceControl = Controller_EvtIoDeviceControl;
	defaultQueueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(wdfDevice,
		&defaultQueueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		0);
	if(!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	Debug("Host created, this is the right place to call:\n\t!wdfpoolusage udetest.sys\n\t!poolused 0 UDEt\n");

	// Initialize virtual USB device software objects.
	// Shown later in this topic.

#if SCENARIO == 1
	// unplug immediately
	for(int i = 0;i < NUM_DEVICES;i++) {
		UDECXUSBDEVICE dev = 0;
		status = Usb_Create(wdfDevice, dev);


		if(NT_SUCCESS(status)) {
			Debug("USB device created & plugged in\n");
			WdfDpcEnqueue(UsbDeviceGetContext(dev)->dpc);
		}
	}

#elif SCENARIO == 2
	// unplug later
	for(int i = 0;i < NUM_DEVICES;i++) {
		UDECXUSBDEVICE dev = 0;
		status = Usb_Create(wdfDevice, dev);

		if(NT_SUCCESS(status)) {
			Debug("USB device created & plugged in\n");
		}
	}
#endif


	return status;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.EvtCleanupCallback = [](_In_ WDFOBJECT ) {};

	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, Controller_WdfEvtDeviceAdd);

	Debug("DriverEntry\n");
	NTSTATUS status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
	);

	if(!NT_SUCCESS(status)) {
		Debug("%s:%d NT error %x\n", __FILE__, __LINE__, status);
		return status;
	}

	return status;
}
