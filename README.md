# FFplay C++

A C++23 rewrite of FFmpeg's ffplay media player with Vulkan-accelerated rendering.

[![CI](https://github.com/aniuzhong/ffplay-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/aniuzhong/ffplay-cpp/actions/workflows/ci.yml)

## Architecture

```mermaid
flowchart LR
    subgraph Input
        SRC["Media File / URL"]
    end

    subgraph Core
        PLR[Player]
        DMX[Demuxer]
        PQT["PacketQueues<br/>videoq · audioq · subq"]
        DEC["Decoders<br/>(per-stream threads)"]
        FQT["FrameQueues<br/>pictq · sampq · subpq"]
    end

    subgraph Pipelines
        VP[VideoPipeline]
        AP[AudioPipeline]
        SP[SubtitlePipeline]
    end

    subgraph Sync
        CLK["Clocks<br/>vidclk · audclk · extclk"]
    end

    subgraph Output
        VO["VideoOutput<br/>SDL · Vulkan · Null"]
        AO["AudioOutput<br/>SDL · Null"]
    end

    subgraph Extras
        AVS[AudioVisualizer]
    end

    SRC --> DMX
    PLR -.->|orchestrates| DMX
    PLR -.-> VP & AP & SP
    PLR -.-> VO & AO

    DMX -->|packets| PQT
    PQT --> DEC
    DEC -->|frames| FQT
    FQT --> VP & AP & SP
    VP --> VO
    AP --> AO
    AP --> AVS
    CLK <--> VP
    CLK <--> AP
```

## Build

```bash
# Windows (MSVC, with Vulkan)
$env:VULKAN_SDK = "C:\VulkanSDK\1.3.xxx.x"
cmake -B build -G "Visual Studio 17 2022" -A x64

# Windows (MSVC, software-only)
cmake -B build -G "Visual Studio 17 2022" -A x64 -DENABLE_VULKAN=OFF

# Linux (Ubuntu 26.04)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=OFF
cmake --build build
```