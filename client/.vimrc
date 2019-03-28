packadd termdebug

function Debug()
	!cd build && make
	if v:shell_error == 0
		TermdebugCommand build/looking-glass-client
	endif
endfunction

command Debug call Debug()
