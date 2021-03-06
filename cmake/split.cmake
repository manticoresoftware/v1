if (__cmake_split_included)
	return ()
endif ()
set (__cmake_split_included YES)

# split debug symbols into separate file

# windows case. The pdbs are located in bin/config/*.pdb
function (__split_win_dbg BINARYNAME DOINSTALL)
	set (PDB_PATH "${CMAKE_CURRENT_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}")
	if (DOINSTALL)
		INSTALL (FILES ${PDB_PATH}/${BINARYNAME}.pdb DESTINATION debug COMPONENT dbgsymbols)
	endif ()
endfunction ()

# Mac OS case. We have to explicitly extract dSYM and then strip the binary
function (__split_apple_dbg BINARYNAME DOINSTALL)
	if (NOT DEFINED CMAKE_DSYMUTIL)
		find_program (CMAKE_DSYMUTIL dsymutil)
	endif ()
	if (NOT DEFINED CMAKE_DSYMUTIL)
		message (SEND_ERROR "Missed objcopy prog. Can't split symbols!")
		unset (SPLIT_SYMBOLS CACHE)
	endif ()
	mark_as_advanced (CMAKE_DSYMUTIL)

	ADD_CUSTOM_COMMAND (TARGET ${BINARYNAME} POST_BUILD
			COMMAND ${CMAKE_DSYMUTIL} -f $<TARGET_FILE:${BINARYNAME}> -o $<TARGET_FILE:${BINARYNAME}>.dSYM
			COMMAND strip -S $<TARGET_FILE:${BINARYNAME}>
			)
	if (DOINSTALL)
		INSTALL (FILES ${MANTICORE_BINARY_DIR}/src/${BINARYNAME}.dSYM
				DESTINATION ${CMAKE_INSTALL_LIBDIR}/debug/usr/bin COMPONENT dbgsymbols)
	endif ()
endfunction ()

# non-windows case. For linux - use objcopy to make 'clean' and 'debug' binaries
function (__split_linux_dbg BINARYNAME DOINSTALL)
	find_package (BinUtils QUIET)
	if (NOT DEFINED CMAKE_OBJCOPY)
		find_program (CMAKE_OBJCOPY objcopy)
	endif ()
	if (NOT DEFINED CMAKE_OBJCOPY)
		message (SEND_ERROR "Missed objcopy prog. Can't split symbols!")
		unset (SPLIT_SYMBOLS CACHE)
	endif (NOT DEFINED CMAKE_OBJCOPY)
	mark_as_advanced (CMAKE_OBJCOPY BinUtils_DIR)

	ADD_CUSTOM_COMMAND (TARGET ${BINARYNAME} POST_BUILD
			COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:${BINARYNAME}> $<TARGET_FILE:${BINARYNAME}>.dbg
			COMMAND ${CMAKE_OBJCOPY} --strip-all $<TARGET_FILE:${BINARYNAME}>
			COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=$<TARGET_FILE:${BINARYNAME}>.dbg $<TARGET_FILE:${BINARYNAME}>
			COMMENT "Splitting symbols from ${BINARYNAME}"
			VERBATIM
			)
	if (DOINSTALL)
		INSTALL (FILES $<TARGET_FILE:${BINARYNAME}>.dbg
				DESTINATION ${CMAKE_INSTALL_LIBDIR}/debug/usr/bin
				COMPONENT dbgsymbols)
	endif ()
endfunction ()

# split debug symbols from target, install them
function (split_dbg BINARYNAME)
	if (MSVC)
		__split_win_dbg (${BINARYNAME} TRUE)
	elseif (APPLE)
		__split_apple_dbg (${BINARYNAME} TRUE)
	else ()
		__split_linux_dbg (${BINARYNAME} TRUE)
	endif ()
endfunction ()
