<div align="center">

<pre>
┏┓┏┓┏┳┓┳┓┏┓
┃ ┣  ┃ ┣┫┣┫
┗┛┗┛ ┻ ┛┗┛┗
</pre>

<h3>
    Cetra Graphics Engine
</h3>

</div>


- Written in C. 
- Minimal dependencies (cglm, glfw, glew, assimp). 
- PBR shader.

---

## Build

```
mkdir build
cd build
cmake ..
make -j 8
```

## Install

The cmake build script does not currently install the libraries system wide.

- The external libraries are installed locally in the ./build/external/lib directory.
- The header files are in the ./build/external/include directory.
- The libcetra library is installed in ./build.

The cmake build script also copies over the models, shaders and textures to the build directory. The render executable expects these to be there.

The `render` exe is built to look for the libraries locally. 

## Run

Simply execute `./render`

~ GLHF


