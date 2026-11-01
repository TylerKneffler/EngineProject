#pragma once
#include "Core/Object.h"
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>

namespace Engine::Scene
{
class ComponentRegistry
{
    using Object=Core::Object;
    using Component=Core::Component;
    template<class T> struct Typed
    {
        uint64_t revision=1;
        std::vector<T*> components;
        std::unordered_map<Object*,std::vector<std::pair<Component*,T*>>> owners;
        // Preserve iterators held by callbacks until the scene's safe boundary.
        std::vector<std::vector<T*>> retired;
        void Replace(Object* owner,std::vector<std::pair<Component*,T*>> next)
        {
            auto found=owners.find(owner);
            if(found==owners.end() && next.empty()) return;
            if(found!=owners.end() && found->second==next) return;
            ++revision;
            retired.push_back(std::move(components)); components=retired.back();
            if(found!=owners.end())
            {
                std::unordered_set<T*> removed;
                for(const auto& pair:found->second) removed.insert(pair.second);
                components.erase(std::remove_if(components.begin(),components.end(),[&](T* value){return removed.count(value)!=0;}),components.end());
            }
            for(const auto& pair:next) components.push_back(pair.second);
            if(next.empty()) owners.erase(owner); else owners[owner]=std::move(next);
        }
        void Refresh(Object* owner,bool removed)
        {
            std::vector<std::pair<Component*,T*>> next;
            if(!removed) for(auto* component:owner->Components)
                if(auto* typed=dynamic_cast<T*>(component)) next.emplace_back(component,typed);
            Replace(owner,std::move(next));
        }
        void Remove(Component* component)
        {
            auto found=owners.find(component->Owner); if(found==owners.end()) return;
            auto next=found->second;
            next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& pair){return pair.first==component;}),next.end());
            Replace(component->Owner,std::move(next));
        }
    };
    struct Entry
    {
        std::shared_ptr<void> data;
        std::function<void(Object*,bool)> refresh;
        std::function<void(Component*)> remove;
        std::function<void()> release;
    };
    mutable std::unordered_map<std::type_index,Entry> m_types;
    mutable std::unordered_set<Object*> m_live;
public:
    bool Contains(Object* owner) const { return m_live.count(owner)!=0; }
    template<class T> const std::vector<T*>& Get(const std::vector<std::unique_ptr<Object>>& objects) const
    {
        auto& entry=m_types[std::type_index(typeid(T))];
        if(!entry.data)
        {
            auto state=std::make_shared<Typed<T>>();
            for(const auto& object:objects)
            {
                m_live.insert(object.get());
                for(auto* component:object->Components) if(auto* value=dynamic_cast<T*>(component))
                { state->components.push_back(value); state->owners[object.get()].emplace_back(component,value); }
            }
            entry.data=state;
            entry.refresh=[state](Object* owner,bool removed){state->Refresh(owner,removed);};
            entry.remove=[state](Component* component){state->Remove(component);};
            entry.release=[state]{state->retired.clear();};
        }
        return std::static_pointer_cast<Typed<T>>(entry.data)->components;
    }
    template<class T> uint64_t Revision(const std::vector<std::unique_ptr<Object>>& objects) const
    { Get<T>(objects); return std::static_pointer_cast<Typed<T>>(m_types.at(std::type_index(typeid(T))).data)->revision; }
    void Adopt(Object* owner)
    { m_live.insert(owner); for(auto& pair:m_types) pair.second.refresh(owner,false); }
    void Changed(Object* owner)
    { if(Contains(owner)) for(auto& pair:m_types) pair.second.refresh(owner,false); }
    void Remove(Object* owner)
    { m_live.erase(owner); for(auto& pair:m_types) pair.second.refresh(owner,true); }
    void Remove(Component* component)
    { if(component && Contains(component->Owner)) for(auto& pair:m_types) pair.second.remove(component); }
    void Reconcile(const std::vector<std::unique_ptr<Object>>& objects)
    {
        std::unordered_set<Object*> live;
        for(const auto& object:objects) live.insert(object.get());
        for(auto* owner:m_live) if(!live.count(owner)) for(auto& pair:m_types) pair.second.refresh(owner,true);
        m_live=std::move(live);
        for(const auto& object:objects) Changed(object.get());
    }
    void ReleaseSnapshots() { for(auto& pair:m_types) pair.second.release(); }
};
}
