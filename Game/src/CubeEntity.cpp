// Registration TU — force-instantiates the static registrars for entity types.
// Template class static members are only instantiated on use, so merely
// including the header is not enough. TNX_REGISTER_ENTITY creates a
// file-scope static that calls PrefabReflector<T<>>::Register().
#include "CubeEntity.h"
#include "MeshProp.h"

TNX_REGISTER_ENTITY (CubeEntity)

TNX_REGISTER_ENTITY (SuperCube)

TNX_REGISTER_ENTITY (MeshProp)