# IMAGE_IMPORT_DESCRIPTOR
	.section	.idata$2
	.global	_head_C__msys64_home_Geoff_LookingGlass_host_release_platform_Windows_capture_DXGI_libd3d11_dll
_head_C__msys64_home_Geoff_LookingGlass_host_release_platform_Windows_capture_DXGI_libd3d11_dll:
	.rva	hname	#Ptr to image import by name list
	#this should be the timestamp, but NT sometimes
	#doesn't load DLLs when this is set.
	.long	0	# loaded time
	.long	0	# Forwarder chain
	.rva	__C__msys64_home_Geoff_LookingGlass_host_release_platform_Windows_capture_DXGI_libd3d11_dll_iname	# imported dll's name
	.rva	fthunk	# pointer to firstthunk
#Stuff for compatibility
	.section	.idata$5
fthunk:
	.section	.idata$4
hname:
