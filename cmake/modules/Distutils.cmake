macro(DISTUTILS_INSTALL_MODULE)
  if(DEFINED ENV{DESTDIR})
    get_filename_component(debian_version /etc/debian_version ABSOLUTE)
    if(EXISTS ${debian_version})
      set(options "--install-layout=deb")
    else()
      set(options "--prefix=/usr")
    endif()
    set(root "--root=$ENV{DESTDIR}")
  endif()

  install(CODE
    "execute_process(COMMAND ${PYTHON_EXECUTABLE} setup.py install ${options} ${root}
                   WORKING_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}\")")
endmacro(DISTUTILS_INSTALL_MODULE)
