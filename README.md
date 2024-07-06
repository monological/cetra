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


## Setup

### On MacOS:

```
brew install glfw cglm glew assimp
```

### On Ubuntu:

```
sudo apt-get install libglfw3-dev libglm-dev libglew-dev libassimp-dev
```

### On Arch:

```
sudo pacman -S glfw cglm glew assimp
```

### On Fedora:

```
sudo dnf install glfw cglm glew assimp
```

## Build

```
make
```

## Run

Example apps are built in the `build/bin` directory.

```
./build/bin/render ./assets/c64.fbx ./assets/c64.fbm 
```



