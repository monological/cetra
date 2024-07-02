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

brew install glfw cglm glew assimp --build-from-source```
make -j 8
```

## Install

The cmake build script does not currently install the libraries system wide.

- The external libraries are installed locally in the ./build/external/lib directory.
- The header files are in the ./build/external/include directory.
- The libcetra library is installed in ./build.

The cmake build script also copies over the models, shaders and textures to the build directory.

The `render` exe is built to look for the models, shaders textures and libraries locally. 

## Run

Simply execute `./render`

~ GLHF


