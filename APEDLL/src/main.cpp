#include <Windows.h>
#include <duktape.h>
#include <vector>

duk_context *ctx = nullptr;

typedef SHORT (WINAPI *GetKeyState_t)(int);
GetKeyState_t getKeyState_orig;

SHORT WINAPI Detour(int key)
{
	SHORT n = getKeyState_orig(key);

	__asm pushad
	__asm pushfd

	__asm popfd
	__asm popad

	return n;
}

template <typename F>
F CreateHook(void* orig, F dest)
{
	DWORD codecave = (DWORD)VirtualAlloc(NULL, 10, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	memcpy((void*)codecave, (void*)orig, 5);
	*(BYTE*)(codecave + 5) = 0xE9;
	*(DWORD*)(codecave + 6) = ((DWORD)orig + 5 - (codecave + 5)) - 5;
	
	// Unprotect address
	DWORD oldProtect;
	VirtualProtect((LPVOID)orig, 5, PAGE_EXECUTE_READWRITE, &oldProtect);

	// JMP codecave
	*(BYTE*)(orig) = 0xE9;
	*(DWORD*)((DWORD)orig + 1) = ((DWORD)dest - (DWORD)orig) - 5;

	VirtualProtect((LPVOID)orig, 5, oldProtect, &oldProtect);

	return (F)codecave;
}

BYTE currentArg;
BYTE numArgs;
DWORD currentOffset;
DWORD currentName;
WORD retn;
int argVal;
__declspec(naked) void WrapJS()
{
	__asm POP EAX
	__asm mov currentName, EAX

	__asm pop EAX
	__asm mov numArgs, AL

	currentArg = numArgs;
	retn = numArgs;

	duk_get_global_string(ctx, (const char*)currentName);

loop_args:

	__asm xor EAX, EAX
	__asm mov AL, currentArg
	__asm test AL, AL
	__asm je call_duktape
	
	__asm mov EAX, 4
	__asm MUL currentArg
	__asm mov EAX, DWORD PTR SS : [EBP + 4 + EAX] // +4 por PUSH EBP
	__asm mov argVal, EAX

	duk_push_int(ctx, argVal);

	__asm dec currentArg
	__asm jmp loop_args
	
call_duktape:
	duk_pcall(ctx, numArgs);

	__asm pop EBP

	// Pop arguments
	__asm pop EAX
	__asm mov currentOffset, EAX

	currentArg = numArgs;

pop_args:
	__asm xor EAX, EAX
	__asm mov AL, currentArg
	__asm test AL, AL
	__asm je retnow;

	__asm pop EAX

	__asm dec currentArg
	__asm jmp pop_args

retnow:
	  // PUSH return
	__asm push currentOffset
	__asm ret
}

duk_ret_t createRedirection(duk_context *ctx) {
	int n = duk_get_top(ctx);  /* #args */
	
	// Number of parameters
	int numArgs = duk_to_number(ctx, 0);

	// Name
	const char* duk_name = duk_to_string(ctx, 1);
	char* name = new char[strlen(duk_name)];
	strcpy(name, duk_name);

	duk_dup(ctx, 2);
	duk_put_global_string(ctx, name);

	// 1 push + 2 mov + 2 push + 5 push + 5 call
	DWORD codecave = (DWORD)VirtualAlloc(NULL, 15, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

	// PUSH EBP
	// MOV EBP, ESP
	*(BYTE*)(codecave + 0) = 0x55;
	*(WORD*)(codecave + 1) = 0xEC8B;

	// PUSH numArgs
	*(BYTE*)(codecave + 3) = 0x6A;
	*(BYTE*)(codecave + 4) = numArgs;

	// PUSH name
	*(BYTE*)(codecave + 5) = 0x68;
	*(DWORD*)(codecave + 6) = (DWORD)name;
	
	// JMP WrapJS
	DWORD currentAddr = codecave + 10;
	*(BYTE*)(currentAddr) = 0xE9;
	*(DWORD*)(currentAddr + 1) = ((DWORD)&WrapJS - currentAddr) - 5;
	

	FARPROC address = GetProcAddress(GetModuleHandleA("user32.dll"), "GetKeyState");
	CreateHook(address, (void*)codecave);

	duk_push_boolean(ctx, true);
	return 1;  /* one return value */
}

/*
duk_ret_t callOriginal(duk_context *ctx) {
	int n = duk_get_top(ctx); 
	int* args = new int[n];
	int retval = 0;

	for (int i = 0; i < n; ++n)
	{
		args[i] = duk_to_number(ctx, i);
	}

	for (int i = n - 1; i >= 0; --i)
	{
		__asm push args[i]
	}

	__asm call org_func
	__asm mov retval, EAX
 
	duk_push_int(ctx, retval);
	return 1; 
}
*/

#include <io.h>
#include <fcntl.h>

bool CreateConsole()
{
	if (AllocConsole())
	{
		long stdioHandle = (long)GetStdHandle(STD_INPUT_HANDLE);
		int consoleHandleR = _open_osfhandle(stdioHandle, _O_TEXT);
		FILE* fptr = _fdopen(consoleHandleR, "r");
		*stdin = *fptr;
		setvbuf(stdin, NULL, _IONBF, 0);

		stdioHandle = (long)GetStdHandle(STD_OUTPUT_HANDLE);
		int consoleHandleW = _open_osfhandle(stdioHandle, _O_TEXT);
		fptr = _fdopen(consoleHandleW, "w");
		*stdout = *fptr;
		setvbuf(stdout, NULL, _IONBF, 1);

		stdioHandle = (long)GetStdHandle(STD_ERROR_HANDLE);
		*stderr = *fptr;
		setvbuf(stderr, NULL, _IONBF, 0);

		// Compatibility for older implementations
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);

		return true;
	}

	return false;
}

BOOL WINAPI DllMain(HINSTANCE handle, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) // Self-explanatory
	{
		CreateConsole();
		DisableThreadLibraryCalls(handle);
		ctx = duk_create_heap_default();

		// Allow setHook to be called from inside JS
		duk_push_c_function(ctx, createRedirection, DUK_VARARGS);
		duk_put_global_string(ctx, "cpp_redirect");

		// DLL API to allow Codecaves, hooks...
		auto jsAPI = \
			"var Redirect = function(callback, identifier) { "				\
			"	identifier = identifier || Math.random().toString(32);"		\
			"	cpp_redirect(callback.length, identifier, callback);"		\
			"};";
		duk_push_object(ctx);
		duk_eval_string(ctx, jsAPI);

		// Custom user code
		auto js = "var customCode = function(key) {"						\
		"	print('Inside hook, key = ' + key); "							\
		"}; "																\
		"Redirect(customCode);";
		duk_push_object(ctx);
		duk_eval_string(ctx, js);
	}

	if (reason == DLL_PROCESS_DETACH) // Self-explanatory
	{
		duk_destroy_heap(ctx);
	}

	return TRUE;
}