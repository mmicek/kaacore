#include "kaacore/resources_manager.h"
#include "kaacore/audio.h"
#include "kaacore/fonts.h"
#include "kaacore/materials.h"
#include "kaacore/shaders.h"
#include "kaacore/textures.h"

namespace kaacore {

ResourcesManager::ResourcesManager()
{
    this->_initialize_resources();
}

ResourcesManager::~ResourcesManager()
{
    this->_uninitialize_resources();
}

void
ResourcesManager::_initialize_resources()
{
    initialize_audio();
    initialize_textures();
    initialize_fonts();
    initialize_shaders();
    initialize_materials();
}

void
ResourcesManager::_uninitialize_resources()
{
    uninitialize_audio();
    uninitialize_textures();
    uninitialize_fonts();
    uninitialize_shaders();
    uninitialize_materials();
}

} // namespace kaacore
