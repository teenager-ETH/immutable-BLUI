
#include "BluEye.h"
#include "RenderHandler.h"

FTickEventLoopData UBluEye::EventLoopData = FTickEventLoopData();

FBluEyeSettings::FBluEyeSettings()
{
	FrameRate = 60.f;

	Width = 1280;
	Height = 720;

	bIsTransparent = false;
	bEnableWebGL = false;
	bAudioMuted = false;
	bAutoPlayEnabled = true;
}

UBluEye::UBluEye(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{
	Texture = nullptr;
	bValidTexture = false;
}

bool UBluEye::Init()
{

	/** 
	 * We don't want this running in editor unless it's PIE
	 * If we don't check this, CEF will spawn infinite processes with widget components
	 **/

	if (GEngine && !GUsingNullRHI)
	{
		if (GEngine->IsEditor() && !GWorld->IsPlayInEditor())
		{
			UE_LOG(LogBlu, Log, TEXT("Notice: not playing - Component Will Not Initialize"));
			return false;
		}
	}
	
	if (Settings.Width <= 0 || Settings.Height <= 0)
	{
		UE_LOG(LogBlu, Log, TEXT("Can't initialize when Width or Height are <= 0"));
		return false;
	}
	
	uint16 DebugPort;
	if(FParse::Value(FCommandLine::Get(), TEXT("cefdebug="), DebugPort))
	{
		// BrowserSettings.remote_debugging_port = DebugPort;
	}

	BrowserSettings.universal_access_from_file_urls = STATE_ENABLED;
	BrowserSettings.file_access_from_file_urls = STATE_ENABLED;

	//BrowserSettings.web_security = STATE_DISABLED;
	//BrowserSettings.fullscreen_enabled = true;

	Info.width = Settings.Width;
	Info.height = Settings.Height;

	// Set transparant option
	Info.SetAsWindowless(0); //bIsTransparent

	// Figure out if we want to turn on WebGL support
	if (Settings.bEnableWebGL)
	{
		if (BluManager::CPURenderSettings)
		{
			UE_LOG(LogBlu, Error, TEXT("You have enabled WebGL for this browser, but CPU Saver is enabled in BluManager.cpp - WebGL will not work!"));
		}
		BrowserSettings.webgl = STATE_ENABLED;
	}

	//NB: this setting will change it globally for all new instances
	BluManager::AutoPlay = Settings.bAutoPlayEnabled;

	Renderer = new RenderHandler(Settings.Width, Settings.Height, this);
	ClientHandler = new BrowserClient(Renderer);
	Browser = CefBrowserHost::CreateBrowserSync(
		Info,
		ClientHandler.get(),
		"about:blank",
		BrowserSettings,
		NULL,
		NULL);

	if (!Browser)
	{
		UE_LOG(LogBlu, Log, TEXT("Failed to create Blu browser!"));

		return false;
	}

	Browser->GetHost()->SetWindowlessFrameRate(Settings.FrameRate);
	Browser->GetHost()->SetAudioMuted(Settings.bAudioMuted);

	// Setup JS event emitter
	ClientHandler->SetEventEmitter(&ScriptEventEmitter);
	ClientHandler->SetLogEmitter(&LogEventEmitter);
	ClientHandler->SetUrlChangeEmitter(&UrlChangeEventEmitter);

	UE_LOG(LogBlu, Log, TEXT("Component Initialized"));
	UE_LOG(LogBlu, Log, TEXT("Loading URL: %s"), *DefaultURL);

	// Load the default URL
	LoadURL(DefaultURL);
	ResetTexture();

	//Instead of manually ticking, we now tick whenever one blu eye is created
	SpawnTickEventLoopIfNeeded();

	return true;
}

void UBluEye::ResetTexture()
{

	// Here we init the texture to its initial state
	DestroyTexture();

	bValidTexture = false;
	Texture = nullptr;
	

	// init the new Texture2D
	Texture = UTexture2D::CreateTransient(Settings.Width, Settings.Height, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();

#if ENGINE_MAJOR_VERSION < 5
	RenderParams.Texture2DResource = (FTexture2DResource*)Texture->Resource;
#else
	RenderParams.Texture2DResource = (FTexture2DResource*)Texture->GetResource();
#endif

	ResetMatInstance();

	bValidTexture = true;
}

void UBluEye::DestroyTexture()
{
	// Here we destroy the texture and its resource
	if (Texture)
	{
		Texture->RemoveFromRoot();

#if ENGINE_MAJOR_VERSION < 5
		if (FTextureResource* Resource = Texture->Resource)
#else
		if (FTextureResource* Resource = Texture->GetResource())
#endif
		{
			BeginReleaseResource(Resource);
			FlushRenderingCommands();
		}

#if ENGINE_MAJOR_VERSION < 5
		Texture->MarkPendingKill();
#else
		Texture->MarkAsGarbage();
#endif
		Texture = nullptr;
		bValidTexture = false;
	}
}

void UBluEye::TextureUpdate(const void *buffer, FUpdateTextureRegion2D *updateRegions, uint32  regionCount)
{
	if (!Browser || !bEnabled)
	{
		UE_LOG(LogBlu, Warning, TEXT("NO BROWSER ACCESS OR NOT ENABLED"))
		return;
	}

	if (bValidTexture && Texture->IsValidLowLevelFast())
	{

		if (buffer == nullptr)
		{
			UE_LOG(LogBlu, Warning, TEXT("NO TEXTDATA"))
				return;
		}
	 
		FUpdateTextureRegionsData* RegionData = new FUpdateTextureRegionsData;
#if ENGINE_MAJOR_VERSION < 5
		RegionData->Texture2DResource = (FTextureResource*)Texture->Resource;
#else
		RegionData->Texture2DResource = (FTextureResource*)Texture->GetResource();
#endif
		RegionData->NumRegions = regionCount;
		RegionData->SrcBpp = 4;
		RegionData->SrcPitch = Settings.Width * 4;
		RegionData->Regions = updateRegions;

		//We need to copy this memory or it might get uninitialized
		RegionData->SrcData.SetNumUninitialized(RegionData->SrcPitch * Settings.Height);
		FPlatformMemory::Memcpy(RegionData->SrcData.GetData(), buffer, RegionData->SrcData.Num());

		if (RegionData->Texture2DResource)
		{
		ENQUEUE_RENDER_COMMAND(UpdateBLUICommand)(
			[RegionData](FRHICommandList& CommandList)
			{
				for (uint32 RegionIndex = 0; RegionIndex < RegionData->NumRegions; RegionIndex++)
				{
					RHIUpdateTexture2D(RegionData->Texture2DResource->TextureRHI->GetTexture2D(), 0, RegionData->Regions[RegionIndex], RegionData->SrcPitch, RegionData->SrcData.GetData()
						+ RegionData->Regions[RegionIndex].SrcY * RegionData->SrcPitch
						+ RegionData->Regions[RegionIndex].SrcX * RegionData->SrcBpp);
				}

				FMemory::Free(RegionData->Regions);
				delete RegionData;
			});
		}

	}
	else {
		UE_LOG(LogBlu, Warning, TEXT("no Texture or Texture->resource"))
	}

}

void UBluEye::ExecuteJS(const FString& Code)
{
	CefString CodeStr = *Code;
	Browser->GetMainFrame()->ExecuteJavaScript(CodeStr, "", 0);
}

void UBluEye::ExecuteJSMethodWithParams(const FString& methodName, const TArray<FString> params)
{

	// Empty param string
	FString ParamString = "(";

	// Build the param string
	for (FString param : params)
	{
		ParamString += param;
		ParamString += ",";
	}
		
	// Remove the last , it's not needed
	ParamString.RemoveFromEnd(",");
	ParamString += ");";

	// time to call the function
	ExecuteJS(methodName + ParamString);
}

void UBluEye::LoadURL(const FString& newURL)
{
	FString FinalUrl = newURL;

	//Detect chrome-devtools, and re-target them to regular devtools
	if (newURL.Contains(TEXT("chrome-devtools://devtools")))
	{
		//devtools://devtools/inspector.html?v8only=true&ws=localhost:9229
		//browser->GetHost()->ShowDevTools(info, g_handler, browserSettings, CefPoint());
		FinalUrl = FinalUrl.Replace(TEXT("chrome-devtools://devtools/bundled/inspector.html"), TEXT("devtools://devtools/inspector.html"));
	}

	// Check if we want to load a local file
	if (newURL.Contains(TEXT("blui://"), ESearchCase::IgnoreCase, ESearchDir::FromStart))
	{

		// Get the current working directory
		FString GameDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

		// We're loading a local file, so replace the proto with our game directory path
		FString LocalFile = newURL.Replace(TEXT("blui://"), *GameDir, ESearchCase::IgnoreCase);

		// Now we use the file proto
		LocalFile = FString(TEXT("file:///")) + LocalFile;

		UE_LOG(LogBlu, Log, TEXT("Load Local File: %s"), *LocalFile)

		// Load it up 
		Browser->GetMainFrame()->LoadURL(*LocalFile);

		return;

	}

	// Load as usual
	Browser->GetMainFrame()->LoadURL(*FinalUrl);


	// TODO
	// Browser->GetMainFrame()->LoadString();

}

void UBluEye::LoadURLWithHeaders(const FString& newURL, TMap<FString, FString> headers)
{
	FString FinalUrl = newURL;

	//Detect chrome-devtools, and re-target them to regular devtools
	if (newURL.Contains(TEXT("chrome-devtools://devtools")))
	{
		//devtools://devtools/inspector.html?v8only=true&ws=localhost:9229
		//browser->GetHost()->ShowDevTools(info, g_handler, browserSettings, CefPoint());
		FinalUrl = FinalUrl.Replace(TEXT("chrome-devtools://devtools/bundled/inspector.html"), TEXT("devtools://devtools/inspector.html"));
	}

	// Check if we want to load a local file
	if (newURL.Contains(TEXT("blui://"), ESearchCase::IgnoreCase, ESearchDir::FromStart))
	{

		// Get the current working directory
		FString GameDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

		// We're loading a local file, so replace the proto with our game directory path
		FString LocalFile = newURL.Replace(TEXT("blui://"), *GameDir, ESearchCase::IgnoreCase);

		// Now we use the file proto
		LocalFile = FString(TEXT("file:///")) + LocalFile;

		UE_LOG(LogBlu, Log, TEXT("Load Local File: %s"), *LocalFile)

		// Load it up
		Browser->GetMainFrame()->LoadURL(*LocalFile);

		return;

	}

	// Create headers to CEF specs
	CefRefPtr<CefRequest> request = CefRequest::Create();
	request->SetURL(*FinalUrl);

	// Initialize a CefRequest::HeaderMap
	CefRequest::HeaderMap headerMap;

	// Iterate through the incoming TMap and populate the CefRequest::HeaderMap
	for (const auto& Pair : headers)
	{
		const FString& Key = Pair.Key;
		const FString& Value = Pair.Value;

		// Convert FString to CefString if necessary
		CefString CefKey = CefString(TCHAR_TO_UTF8(*Key));
		CefString CefValue = CefString(TCHAR_TO_UTF8(*Value));

		// Add key-value pair to the CefRequest::HeaderMap
		headerMap.insert(std::make_pair(CefKey, CefValue));
	}

	request->SetHeaderMap(headerMap);

    /*
     * Needs to be used AFTER LoadURL to prevent rpc INVALID_INITIATOR_ORIGIN error
     * see: cef_frame.h
    */
	Browser->GetMainFrame()->LoadRequest(request);

}


FString UBluEye::GetCurrentURL()
{
	return FString(Browser->GetMainFrame()->GetURL().c_str());
}

void UBluEye::SetZoom(const float Scale /*= 1*/)
{
	Browser->GetHost()->SetZoomLevel(Scale);
}

float UBluEye::GetZoom()
{
	return Browser->GetHost()->GetZoomLevel();
}

void UBluEye::DownloadFile(const FString& FileUrl)
{
	Browser->GetHost()->StartDownload(*FileUrl);
	//Todo: ensure downloading works in some way, shape or form?
}

bool UBluEye::IsBrowserLoading()
{
	return Browser->IsLoading();
}

void UBluEye::ReloadBrowser(bool IgnoreCache)
{

	if (IgnoreCache)
	{
		return Browser->ReloadIgnoreCache();
	}

	Browser->Reload();

}

void UBluEye::NavBack()
{

	if (Browser->CanGoBack())
	{
		Browser->GoBack();
	}

}

void UBluEye::NavForward()
{

	if (Browser->CanGoForward())
	{
		Browser->GoForward();
	}

}

UTexture2D* UBluEye::ResizeBrowser(const int32 NewWidth, const int32 NewHeight)
{

	if (NewWidth <= 0 || NewHeight <= 0)
	{
		// We can't do this, just do nothing.
		UE_LOG(LogBlu, Log, TEXT("Can't resize when one or both of the sizes are <= 0!"));
		return Texture;
	}

	// Disable the web view while we resize
	bEnabled = false;

	// Set our new Width and Height
	Settings.Width = NewWidth;
	Settings.Height = NewHeight;
	
	// Update our render handler
	Renderer->Width = NewWidth;
	Renderer->Height = NewHeight;

	bValidTexture = false;

	Texture = UTexture2D::CreateTransient(Settings.Width, Settings.Height, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();

	bValidTexture = true;

	// Let the browser's host know we resized it
	Browser->GetHost()->WasResized();

	// Now we can keep going
	bEnabled = true;

	UE_LOG(LogBlu, Log, TEXT("BluEye was resized!"))

	return Texture;

}

UTexture2D* UBluEye::CropWindow(const int32 Y, const int32 X, const int32 NewWidth, const int32 NewHeight)
{
	// Disable the web view while we resize
	bEnabled = false;


	// Set our new Width and Height
	Settings.Width = NewWidth;
	Settings.Height = NewHeight;

	// Update our render handler
	Renderer->Width = NewWidth;
	Renderer->Height = NewHeight;

	bValidTexture = false;

	Texture = UTexture2D::CreateTransient(Settings.Width, Settings.Height, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();

	bValidTexture = true;

	// Now we can keep going
	bEnabled = true;

	UE_LOG(LogBlu, Log, TEXT("BluEye was cropped!"))

	return Texture;
}

UBluEye* UBluEye::SetProperties(const int32 SetWidth,
	const int32 SetHeight,
	const bool SetIsTransparent,
	const bool SetEnabled,
	const bool SetWebGL,
	const FString& SetDefaultURL,
	const FName& SetTextureParameterName,
	UMaterialInterface* SetBaseMaterial)
{
	Settings.Width = SetWidth;
	Settings.Height = SetHeight;

	bEnabled = SetEnabled;

	Settings.bIsTransparent = SetIsTransparent;
	Settings.bEnableWebGL = SetWebGL;
	BaseMaterial = SetBaseMaterial;

	DefaultURL = SetDefaultURL;
	TextureParameterName = SetTextureParameterName;

	return this;
}

void UBluEye::TriggerMouseMove(const FVector2D& Pos, const float Scale)
{

	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendFocusEvent(true);
	Browser->GetHost()->SendMouseMoveEvent(MouseEvent, false);

}

void UBluEye::TriggerLeftClick(const FVector2D& Pos, const float Scale)
{
	TriggerLeftMouseDown(Pos, Scale);
	TriggerLeftMouseUp(Pos, Scale);
}

void UBluEye::TriggerRightClick(const FVector2D& Pos, const float Scale)
{
	TriggerRightMouseDown(Pos, Scale);
	TriggerRightMouseUp(Pos, Scale);
}

void UBluEye::TriggerLeftMouseDown(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_LEFT, false, 1);
}

void UBluEye::TriggerRightMouseDown(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_RIGHT, false, 1);
}

void UBluEye::TriggerLeftMouseUp(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_LEFT, true, 1);
}

void UBluEye::TriggerRightMouseUp(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_RIGHT, true, 1);
}

void UBluEye::TriggerMouseWheel(const float MouseWheelDelta, const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseWheelEvent(MouseEvent, MouseWheelDelta * 10, MouseWheelDelta * 10);
}

void UBluEye::KeyDown(FKeyEvent InKey)
{

	ProcessKeyMods(InKey);
	ProcessKeyCode(InKey);

	KeyEvent.type = KEYEVENT_KEYDOWN;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

}

void UBluEye::KeyUp(FKeyEvent InKey)
{

	ProcessKeyMods(InKey);
	ProcessKeyCode(InKey);

	KeyEvent.type = KEYEVENT_KEYUP;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

}

void UBluEye::KeyPress(FKeyEvent InKey)
{

	// Simply trigger down, then up key events
	KeyDown(InKey);
	KeyUp(InKey);

}

void UBluEye::ProcessKeyCode(FKeyEvent InKey)
{
	KeyEvent.native_key_code = InKey.GetKeyCode();
	KeyEvent.windows_key_code = InKey.GetKeyCode();
}

void UBluEye::CharKeyInput(FCharacterEvent CharEvent)
{

	// Process keymods like usual
	ProcessKeyMods(CharEvent);

	// Below char input needs some special treatment, se we can't use the normal key down/up methods

#if PLATFORM_MAC
	KeyEvent.character = CharEvent.GetCharacter();
#else
    KeyEvent.windows_key_code = CharEvent.GetCharacter();
    KeyEvent.native_key_code = CharEvent.GetCharacter();
#endif
	KeyEvent.type = KEYEVENT_CHAR;
	Browser->GetHost()->SendKeyEvent(KeyEvent);
}

void UBluEye::CharKeyDownUp(FCharacterEvent CharEvent)
{
	// Process keymods like usual
	ProcessKeyMods(CharEvent);

	// Below char input needs some special treatment, se we can't use the normal key down/up methods

#if PLATFORM_MAC
	KeyEvent.character = CharEvent.GetCharacter();
#else
	KeyEvent.windows_key_code = CharEvent.GetCharacter();
	KeyEvent.native_key_code = CharEvent.GetCharacter();
#endif
	KeyEvent.type = KEYEVENT_KEYDOWN;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

	KeyEvent.type = KEYEVENT_KEYUP;
	Browser->GetHost()->SendKeyEvent(KeyEvent);
}

void UBluEye::RawCharKeyPress(const FString CharToPress, bool isRepeat,
	bool LeftShiftDown,
	bool RightShiftDown,
	bool LeftControlDown,
	bool RightControlDown,
	bool LeftAltDown,
	bool RightAltDown,
	bool LeftCommandDown,
	bool RightCommandDown,
	bool CapsLocksOn)
{

	FModifierKeysState* KeyState = new FModifierKeysState(LeftShiftDown, RightShiftDown, LeftControlDown, 
		RightControlDown, LeftAltDown, RightAltDown, LeftCommandDown, RightCommandDown, CapsLocksOn);

	FCharacterEvent* CharEvent = new FCharacterEvent(CharToPress.GetCharArray()[0], *KeyState, 0, isRepeat);

	CharKeyInput(*CharEvent);

}

void UBluEye::SpecialKeyPress(EBluSpecialKeys Key, bool LeftShiftDown,
	bool RightShiftDown,
	bool LeftControlDown,
	bool RightControlDown,
	bool LeftAltDown,
	bool RightAltDown,
	bool LeftCommandDown,
	bool RightCommandDown,
	bool CapsLocksOn)
{

	int32 KeyValue = Key;

	KeyEvent.windows_key_code = KeyValue;
	KeyEvent.native_key_code = KeyValue;
	KeyEvent.type = KEYEVENT_KEYDOWN;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

	KeyEvent.windows_key_code = KeyValue;
	KeyEvent.native_key_code = KeyValue;
	// bits 30 and 31 should be always 1 for WM_KEYUP
	KeyEvent.type = KEYEVENT_KEYUP;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

}

void UBluEye::ProcessKeyMods(FInputEvent InKey)
{

	int Mods = 0;

	// Test alt
	if (InKey.IsAltDown())
	{
		Mods |= cef_event_flags_t::EVENTFLAG_ALT_DOWN;
	}
	else
	// Test control
	if (InKey.IsControlDown())
	{
		Mods |= cef_event_flags_t::EVENTFLAG_CONTROL_DOWN;
	} 
	else
	// Test shift
	if (InKey.IsShiftDown())
	{
		Mods |= cef_event_flags_t::EVENTFLAG_SHIFT_DOWN;
	}

	KeyEvent.modifiers = Mods;

}

void UBluEye::SpawnTickEventLoopIfNeeded()
{
	if (!EventLoopData.DelegateHandle.IsValid())
	{
		FTickerDelegate TickDelegate = FTickerDelegate::CreateLambda([](float DeltaTime)
		{
			if (EventLoopData.bShouldTickEventLoop)
			{
				//UE_LOG(LogTemp, Log, TEXT("Delta: %1.2f"), DeltaTime);
				BluManager::DoBluMessageLoop();
			}

			return true;
		});

#if ENGINE_MAJOR_VERSION < 5
		EventLoopData.DelegateHandle = FTicker::GetCoreTicker().AddTicker(TickDelegate);
#else
		EventLoopData.DelegateHandle = FTSTicker::GetCoreTicker().AddTicker(TickDelegate);
#endif
	}

	EventLoopData.EyeCount++;
}

UTexture2D* UBluEye::GetTexture() const
{
	if (!Texture)
	{
		return UTexture2D::CreateTransient(Settings.Width, Settings.Height, PF_B8G8R8A8);
	}

	return Texture;
}

void UBluEye::ResetMatInstance()
{
	if (!Texture || !BaseMaterial || TextureParameterName.IsNone())
	{
		return;
	}

	// Create material instance
	if (!MaterialInstance)
	{
		MaterialInstance = UMaterialInstanceDynamic::Create(BaseMaterial, NULL);
		if (!MaterialInstance)
		{
			UE_LOG(LogBlu, Warning, TEXT("UI Material instance can't be created"));
			return;
		}
	}

	// Check again, we must have material instance
	if (!MaterialInstance)
	{
		UE_LOG(LogBlu, Error, TEXT("UI Material instance wasn't created"));
		return;
	}

	// Check we have desired parameter
	UTexture* Tex = nullptr;
	if (!MaterialInstance->GetTextureParameterValue(TextureParameterName, Tex))
	{
		UE_LOG(LogBlu, Warning, TEXT("UI Material instance Texture parameter not found"));
		return;
	}

	MaterialInstance->SetTextureParameterValue(TextureParameterName, Texture);
}

void UBluEye::CloseBrowser()
{
	BeginDestroy();
}

void UBluEye::BeginDestroy()
{
	if (Browser)
	{
		Browser->GetHost()->TryCloseBrowser();
		Browser = nullptr;
		UE_LOG(LogBlu, Log, TEXT("Browser Closing"));
	}

	DestroyTexture();
	SetFlags(RF_BeginDestroyed);

	//Remove our auto-ticking setup
	EventLoopData.EyeCount--;
	if (EventLoopData.EyeCount <= 0)
	{
#if ENGINE_MAJOR_VERSION < 5
		FTicker::GetCoreTicker().RemoveTicker(EventLoopData.DelegateHandle);
#else
		FTSTicker::GetCoreTicker().RemoveTicker(EventLoopData.DelegateHandle);
#endif
		EventLoopData.DelegateHandle.Reset();
	}
	Super::BeginDestroy();
}

void UBluEye::SetShouldTickEventLoop(bool ShouldTick /*= true*/)
{
	EventLoopData.bShouldTickEventLoop = ShouldTick;
}
