#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Rendering
{
// A small per-frame dependency plan. Resource ids are logical handles owned by
// the caller; this class orders passes but leaves API resources and commands to
// the active renderer backend.
class RenderFramePlan
{
public:
    using ResourceId = uint32_t;

    enum class Access : uint8_t
    {
        Read,
        Write,
        ReadWrite
    };

    enum class Owner : uint8_t
    {
        Application,
        Renderer
    };

    struct ResourceUse
    {
        ResourceId resource = 0;
        Access access = Access::Read;
    };

    struct PassInfo
    {
        std::string name;
        Owner owner = Owner::Application;
        std::vector<size_t> dependencies;
    };

    size_t AddPass(std::string name,
        std::initializer_list<ResourceUse> resources,
        std::function<void()> execute)
    {
        return AddPass(std::move(name), std::vector<ResourceUse>(resources),
            std::move(execute));
    }

    size_t AddPass(std::string name, std::vector<ResourceUse> resources,
        std::function<void()> execute)
    {
        if (!execute)
            throw std::invalid_argument("Application frame-plan pass needs an executor");
        passes_.push_back({ PassInfo{ std::move(name), Owner::Application, {} },
            std::move(resources), std::move(execute) });
        compiled_ = false;
        return passes_.size() - 1;
    }

    size_t AddRendererPass(std::string name,
        std::initializer_list<ResourceUse> resources)
    {
        return AddRendererPass(std::move(name), std::vector<ResourceUse>(resources));
    }

    size_t AddRendererPass(std::string name, std::vector<ResourceUse> resources)
    {
        passes_.push_back({ PassInfo{ std::move(name), Owner::Renderer, {} },
            std::move(resources), {} });
        compiled_ = false;
        return passes_.size() - 1;
    }

    void Compile()
    {
        compiledOrder_.clear();
        std::vector<size_t> lastWriter;
        std::vector<std::vector<size_t>> readers;

        for (size_t passIndex = 0; passIndex < passes_.size(); ++passIndex)
        {
            auto& pass = passes_[passIndex];
            pass.info.dependencies.clear();
            for (const ResourceUse& use : pass.resources)
            {
                const size_t resourceIndex = static_cast<size_t>(use.resource);
                if (lastWriter.size() <= resourceIndex)
                {
                    lastWriter.resize(resourceIndex + 1, InvalidPass);
                    readers.resize(resourceIndex + 1);
                }

                if (use.access == Access::Read)
                {
                    AddDependency(pass.info.dependencies, lastWriter[resourceIndex]);
                    readers[resourceIndex].push_back(passIndex);
                    continue;
                }

                AddDependency(pass.info.dependencies, lastWriter[resourceIndex]);
                for (size_t reader : readers[resourceIndex])
                    AddDependency(pass.info.dependencies, reader);
                readers[resourceIndex].clear();
                lastWriter[resourceIndex] = passIndex;
            }
        }

        std::vector<size_t> remainingDependencies(passes_.size());
        std::vector<std::vector<size_t>> dependents(passes_.size());
        for (size_t passIndex = 0; passIndex < passes_.size(); ++passIndex)
        {
            remainingDependencies[passIndex] = passes_[passIndex].info.dependencies.size();
            for (size_t dependency : passes_[passIndex].info.dependencies)
                dependents[dependency].push_back(passIndex);
        }

        std::vector<bool> emitted(passes_.size(), false);
        while (compiledOrder_.size() < passes_.size())
        {
            size_t ready = InvalidPass;
            for (size_t passIndex = 0; passIndex < passes_.size(); ++passIndex)
                if (!emitted[passIndex] && !remainingDependencies[passIndex])
                {
                    ready = passIndex;
                    break;
                }
            if (ready == InvalidPass)
                throw std::logic_error("Render frame plan contains a dependency cycle");

            emitted[ready] = true;
            compiledOrder_.push_back(ready);
            for (size_t dependent : dependents[ready])
                --remainingDependencies[dependent];
        }
        compiled_ = true;
    }

    void Execute()
    {
        if (!compiled_) Compile();
        // Renderer-owned entries describe ordering/resource use for work that
        // the renderer performs outside application callbacks (for example,
        // the editor UI overlay after RenderIfNeeded's draw callback returns).
        for (size_t passIndex : compiledOrder_)
            if (passes_[passIndex].info.owner == Owner::Application)
                passes_[passIndex].execute();
    }

    const std::vector<size_t>& GetExecutionOrder()
    {
        if (!compiled_) Compile();
        return compiledOrder_;
    }

    const PassInfo& GetPass(size_t index) const { return passes_.at(index).info; }
    size_t GetPassCount() const { return passes_.size(); }

private:
    static constexpr size_t InvalidPass = static_cast<size_t>(-1);

    static void AddDependency(std::vector<size_t>& dependencies, size_t passIndex)
    {
        if (passIndex != InvalidPass &&
            std::find(dependencies.begin(), dependencies.end(), passIndex) == dependencies.end())
            dependencies.push_back(passIndex);
    }

    struct Pass
    {
        PassInfo info;
        std::vector<ResourceUse> resources;
        std::function<void()> execute;
    };

    std::vector<Pass> passes_;
    std::vector<size_t> compiledOrder_;
    bool compiled_ = false;
};
}
