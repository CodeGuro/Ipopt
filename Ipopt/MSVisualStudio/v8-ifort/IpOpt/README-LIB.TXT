The DLLs in this directory export the C interface of the Ipopt solver, as well as its C++ interface 
(which can be accessed via the IpoptApplicationFactory method). By linking against the import library 
you can use these DLLs from within any code compiled using the MSVC and/or Intel compilers for the 
Windows platform.

The DLLs include the MUMPS solver by default, and can dynamically load a separate DLL containing the 
(non-free) HSL solvers if available. If you have access to the HSL solver sources, you have to compile 
the DLL for the HSL solvers yourself. The v8-ifort directory in the Ipopt source distribution contains 
a separate libhsl project to create the DLL using the Intel fortran compiler. All DLLs are statically 
linked, so do not require any other runtime DLLs. 
 
To use these DLLs, you need to add the include\coin directory to the include path of your MSVC project.
In addition, you have to make sure that HAVE_CONFIG_H is defined, and that the Ipopt import library for 
your platform is added to list of linker inputs of your project. To run your program, you have to copy the 
correct Ipopt DLL for your platform and configuration to the directory where your executable is located. 

Please note that the debug and release versions of the Ipopt DLL are *not* binary compatible, because 
the MSVC compiler unfortunately treats std::string objects slightly differently in both configurations. 
Using a release DLL in debug code and vice versa will therefore propably lead to stack corruption, runtime 
errors, or other hard-to-explain crashes. 
