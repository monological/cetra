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

The cmake build script does not currently install all the libraries system wide.

- The external libraries are installed locally in the ./build/external/lib directory.
- The header files are in the ./build/external/include directory.
- The libcetra library is installed in ./build.

The `render` exe is built to look for the libraries locally. 

## Run

Simply execute `./render`

~ GLHF


