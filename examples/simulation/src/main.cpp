#include <dk/render/ClothRenderer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
template<class T,class E> T take(std::expected<T,E>&& value) {
    if (!value) {
        if constexpr (std::is_same_v<E,dk::Error>) {
            auto text=value.error().message; for (const auto& context:value.error().context) text+=" / "+context;
            throw std::runtime_error(text);
        } else throw std::runtime_error("memory operation failed");
    }
    return std::move(*value);
}
void save(const dk::render::SimulationFrame& frame,const std::filesystem::path& path) {
    std::vector<std::byte> pixels(std::size_t{frame.width()}*frame.height()*4);
    auto read=frame.read_rgba8(pixels); if (!read) throw std::runtime_error(read.error().message);
    std::ofstream file(path,std::ios::binary);
    file<<"P6\n"<<frame.width()<<' '<<frame.height()<<"\n255\n";
    for (std::size_t i=0;i<pixels.size();i+=4) file.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    if (!file) throw std::runtime_error("cannot write image");
}
}
int main(int argc,char** argv) {
    if (argc>2 || (argc==2 && std::string_view(argv[1])=="--help")) {
        std::cout<<"Usage: dk-xpbd-demo [output-directory]\nSeed 42, 8x8 cloth, dt=10ms; writes initial and 300-step PPM images.\n";
        return argc>2 ? 2 : 0;
    }
    try {
        const auto directory=std::filesystem::absolute(argc==2 ? argv[1] : "xpbd-demo");
        std::filesystem::create_directories(directory);
        auto memory=take(dk::memory::MemorySystem::create());
        auto heap=take(memory.create_heap({"XPBD demo",dk::memory::DomainCategory::render}));
        dk::memory::ThreadContext thread{memory}; dk::memory::ExecutionScope scope{thread,heap};
        auto device=take(dk::graphics::Device::create(heap));
        auto queue=take(dk::graphics::SubmissionQueue::create(heap,std::move(device)));
        dk::ClothConfig config; config.seed=42;
        auto cpu=take(dk::XpbdSolver::cloth(config));
        auto solver=take(dk::GpuXpbdSolver::create(heap,queue,cpu,DK_XPBD_SHADER));
        auto renderer=take(dk::render::ClothRenderer::create(heap,queue,DK_CLOTH_SHADER));
        dk::render::ClothView view; view.image_readback=true;
        auto first=take(renderer.render(queue,solver,10000000,0,view));
        if (!take(first.physics().wait(queue))) throw std::runtime_error("GPU wait timed out");
        save(first,directory/"cloth-initial.ppm"); first={};
        for (unsigned step=0;step<296;step+=8) {
            auto frame=take(solver.advance(queue,10000000,8));
            if (!take(frame.wait(queue))) throw std::runtime_error("GPU wait timed out");
        }
        auto last=take(renderer.render(queue,solver,10000000,4,view));
        if (!take(last.physics().wait(queue))) throw std::runtime_error("GPU wait timed out");
        save(last,directory/"cloth-300.ppm");
        std::cout<<"steps="<<last.physics().steps()<<" simulated_time_ns=3000000000 output="<<directory.string()<<'\n';
        auto closed=queue.close(); if (!closed) throw std::runtime_error(closed.error().message);
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
