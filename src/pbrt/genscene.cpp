
#include <pbrt/genscene.h>

#include <pbrt/cpurender.h>
#include <pbrt/materials.h>
#include <pbrt/options.h>
#include <pbrt/shapes.h>  // WritePlyFile
#include <pbrt/paramdict.h>
#include <pbrt/util/args.h>
#include <pbrt/util/color.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/spectrum.h>
#include <pbrt/util/parallel.h>
#include <pbrt/util/profile.h>

#include <iostream>

namespace pbrt {

GeneralSceneBase::~GeneralSceneBase() {}

template <typename T, typename U>
static std::string ToString(const std::map<T, U> &m) {
    std::string s = "[ ";
    for (const auto &iter : m)
        s += StringPrintf("%s:%s ", iter.first, iter.second);
    s += "]";
    return s;
}

template <typename T, typename U>
static std::string ToString(const std::pair<T, U> &p) {
    return StringPrintf("[ std::pair first: %s second: %s ]", p.first, p.second);
}

std::string GeneralScene::ToString() const {
    return StringPrintf("[ GeneralScene camera: %s film: %s sampler: %s integrator: %s "
                        "filter: %s accelerator: %s namedMaterials: %s materials: %s "
                        "media: %s floatTextures: %s spectrumTextures: %s "
                        "instanceDefinitions: %s lights: %s "
                        "shapes: %s instances: %s ]", camera, film, sampler, integrator,
                        filter, accelerator, namedMaterials, materials, media,
                        floatTextures, spectrumTextures, instanceDefinitions, lights,
                        shapes, instances);
}

// API Local Classes

struct GeneralScene::GraphicsState {
    // Graphics State Methods
    GraphicsState();

    // Graphics State
    std::string currentInsideMedium, currentOutsideMedium;

    int currentMaterialIndex = 0;
    std::string currentMaterialName;

    std::string areaLightName;
    ParameterDictionary areaLightParams;
    FileLoc areaLightLoc;

    const RGBColorSpace *colorSpace = RGBColorSpace::sRGB;

    bool reverseOrientation = false;

    ParsedParameterVector shapeAttributes;
    ParsedParameterVector lightAttributes;
    ParsedParameterVector materialAttributes;
    ParsedParameterVector mediumAttributes;
    ParsedParameterVector textureAttributes;
};

// API Static Data

GeneralScene::GraphicsState::GraphicsState() {
    currentMaterialIndex = 0;
}

// API Macros
#define VERIFY_INITIALIZED(func)                           \
    if (currentApiState == APIState::Uninitialized) {        \
        Error(&loc,                                          \
              "pbrtInit() must be before calling \"%s()\". " \
              "Ignoring.",                                   \
              func);                                         \
        return;                                              \
    } else /* swallow trailing semicolon */
#define VERIFY_OPTIONS(func)                                            \
    VERIFY_INITIALIZED(func);                                           \
    if (currentApiState == APIState::WorldBlock) {                      \
        Error(&loc,                                                     \
              "Options cannot be set inside world block; "      \
              "\"%s\" not allowed.  Ignoring.",                 \
              func);                                            \
        return;                                                 \
    } else /* swallow trailing semicolon */
#define VERIFY_WORLD(func)                                   \
    VERIFY_INITIALIZED(func);                                \
    if (currentApiState == APIState::OptionsBlock) {                    \
        Error(&loc,                                                     \
              "Scene description must be inside world block; "          \
              "\"%s\" not allowed. Ignoring.",                          \
              func);                                                    \
        return;                                                         \
    } else /* swallow trailing semicolon */
#define FOR_ACTIVE_TRANSFORMS(expr)           \
    for (int i = 0; i < MaxTransforms; ++i)   \
        if (activeTransformBits & (1 << i)) { \
            expr                              \
        }
#define WARN_IF_ANIMATED_TRANSFORM(func)                                \
    do {                                                                \
        if (curTransform.IsAnimated())                                  \
            Warning(&loc,                                               \
                    "Animated transformations set; ignoring for \"%s\" " \
                    "and using the start transform only",               \
                    func);                                              \
    } while (false) /* swallow trailing semicolon */

STAT_MEMORY_COUNTER("Memory/TransformCache", transformCacheBytes);
STAT_PERCENT("Scene/TransformCache hits", nTransformCacheHits, nTransformCacheLookups);

const Transform *TransformCache::Lookup(const Transform &t) {
    ++nTransformCacheLookups;

    if (!hashTable.empty()) {
        size_t offset = t.Hash() % hashTable.bucket_count();
        for (auto iter = hashTable.begin(offset); iter != hashTable.end(offset);
             ++iter) {
            if (**iter == t) {
                ++nTransformCacheHits;
                return *iter;
            }
        }
    }
    Transform *tptr = alloc.new_object<Transform>(t);
    transformCacheBytes += sizeof(Transform);
    hashTable.insert(tptr);
    return tptr;
}

TransformCache::~TransformCache() {
    for (const auto &iter : hashTable) {
        Transform *tptr = iter;
        alloc.delete_object(tptr);
    }
}

GeneralScene::GeneralScene()
    : transformCache(Allocator(&transformMemoryResource)) {
    currentApiState = APIState::OptionsBlock;
    graphicsState = new GraphicsState;

    // Defaults
    ParameterDictionary dict({}, RGBColorSpace::sRGB);
    materials.push_back(GeneralSceneEntity("diffuse", dict, {}));

    filter.name = "gaussian";
    film.name = "rgb";
}

GeneralScene::~GeneralScene() {
    delete graphicsState;
}

void GeneralScene::Option(const std::string &name, const std::string &value,
                          FileLoc loc) {
    std::string nName = normalizeArg(name);

    VERIFY_INITIALIZED("Option");
    if (nName == "disablepixeljitter") {
        if (value == "true")
            PbrtOptions.disablePixelJitter = true;
        else if (value == "false")
            PbrtOptions.disablePixelJitter = false;
        else
            ErrorExit(&loc, "%s: expected \"true\" or \"false\" for option value",
                      value);
    } else if (nName == "disablewavelengthjitter") {
        if (value == "true")
            PbrtOptions.disableWavelengthJitter = true;
        else if (value == "false")
            PbrtOptions.disableWavelengthJitter = false;
        else
            ErrorExit(&loc, "%s: expected \"true\" or \"false\" for option value",
                      value);
    } else if (nName == "msereferenceimage") {
        if (value.size() < 3 || value.front() != '"' || value.back() != '"')
            ErrorExit(&loc, "%s: expected quoted string for option value",
                      value);
        PbrtOptions.mseReferenceImage = value.substr(1, value.size() - 2);
    } else if (nName == "msereferenceout") {
        if (value.size() < 3 || value.front() != '"' || value.back() != '"')
            ErrorExit(&loc, "%s: expected quoted string for option value",
                      value);
        PbrtOptions.mseReferenceOutput = value.substr(1, value.size() - 2);
    } else if (nName == "seed") {
        PbrtOptions.seed = std::atoi(value.c_str());
    } else if (nName == "forcediffuse") {
        if (value == "true")
            PbrtOptions.forceDiffuse = true;
        else if (value == "false")
            PbrtOptions.forceDiffuse = false;
        else
            ErrorExit(&loc, "%s: expected \"true\" or \"false\" for option value",
                      value);
    } else if (nName == "pixelstats") {
        if (value == "true")
            PbrtOptions.recordPixelStatistics = true;
        else if (value == "false")
            PbrtOptions.recordPixelStatistics = false;
        else
            ErrorExit(&loc, "%s: expected \"true\" or \"false\" for option value",
                      value);
    } else
        ErrorExit(&loc, "%s: unknown option", name);
}

void GeneralScene::Identity(FileLoc loc) {
    VERIFY_INITIALIZED("Identity");
    FOR_ACTIVE_TRANSFORMS(curTransform[i] = pbrt::Transform();)
}

void GeneralScene::Translate(Float dx, Float dy, Float dz, FileLoc loc) {
    VERIFY_INITIALIZED("Translate");
    FOR_ACTIVE_TRANSFORMS(curTransform[i] = curTransform[i] *
                          pbrt::Translate(Vector3f(dx, dy, dz));)
}

void GeneralScene::Transform(Float tr[16], FileLoc loc) {
    VERIFY_INITIALIZED("Transform");
    FOR_ACTIVE_TRANSFORMS(curTransform[i] =
                          Transpose(pbrt::Transform(SquareMatrix<4>(pstd::MakeSpan(tr, 16))));)
}

void GeneralScene::ConcatTransform(Float tr[16], FileLoc loc) {
    VERIFY_INITIALIZED("ConcatTransform");
    FOR_ACTIVE_TRANSFORMS(
        curTransform[i] = curTransform[i] *
        Transpose(pbrt::Transform(SquareMatrix<4>(pstd::MakeSpan(tr, 16))));)
}

void GeneralScene::Rotate(Float angle, Float dx, Float dy, Float dz, FileLoc loc) {
    VERIFY_INITIALIZED("Rotate");
    FOR_ACTIVE_TRANSFORMS(curTransform[i] =
                          curTransform[i] *
                          pbrt::Rotate(angle, Vector3f(dx, dy, dz));)
}

void GeneralScene::Scale(Float sx, Float sy, Float sz, FileLoc loc) {
    VERIFY_INITIALIZED("Scale");
    FOR_ACTIVE_TRANSFORMS(curTransform[i] =
                          curTransform[i] * pbrt::Scale(sx, sy, sz);)
}

void GeneralScene::LookAt(Float ex, Float ey, Float ez, Float lx, Float ly, Float lz,
                Float ux, Float uy, Float uz, FileLoc loc) {
    VERIFY_INITIALIZED("LookAt");
    class Transform lookAt =
        pbrt::LookAt(Point3f(ex, ey, ez), Point3f(lx, ly, lz), Vector3f(ux, uy, uz));
    FOR_ACTIVE_TRANSFORMS(curTransform[i] = curTransform[i] * lookAt;);
}

void GeneralScene::CoordinateSystem(const std::string &name, FileLoc loc) {
    VERIFY_INITIALIZED("CoordinateSystem");
    namedCoordinateSystems[name] = curTransform;
}

void GeneralScene::CoordSysTransform(const std::string &name, FileLoc loc) {
    VERIFY_INITIALIZED("CoordSysTransform");
    if (namedCoordinateSystems.find(name) != namedCoordinateSystems.end())
        curTransform = namedCoordinateSystems[name];
    else
        Warning(&loc, "Couldn't find named coordinate system \"%s\"", name);
}

void GeneralScene::ActiveTransformAll(FileLoc loc) {
    activeTransformBits = AllTransformsBits;
}

void GeneralScene::ActiveTransformEndTime(FileLoc loc) {
    activeTransformBits = EndTransformBits;
}

void GeneralScene::ActiveTransformStartTime(FileLoc loc) {
    activeTransformBits = StartTransformBits;
}

void GeneralScene::TransformTimes(Float start, Float end, FileLoc loc) {
    VERIFY_OPTIONS("TransformTimes");
    transformStartTime = start;
    transformEndTime = end;
}

void GeneralScene::ColorSpace(const std::string &n, FileLoc loc) {
    VERIFY_INITIALIZED("RGBColorSpace");
    if (const RGBColorSpace *cs = RGBColorSpace::GetNamed(n))
        graphicsState->colorSpace = cs;
    else
        Error(&loc, "%s: color space unknown", n);
}

void GeneralScene::PixelFilter(const std::string &name, ParsedParameterVector params,
                     FileLoc loc) {
    ParameterDictionary dict(std::move(params), graphicsState->colorSpace);
    VERIFY_OPTIONS("PixelFilter");
    filter = GeneralSceneEntity(name, std::move(dict), loc);
}

void GeneralScene::Film(const std::string &type, ParsedParameterVector params,
              FileLoc loc) {
    ParameterDictionary dict(std::move(params), graphicsState->colorSpace);
    VERIFY_OPTIONS("Film");
    film = GeneralSceneEntity(type, std::move(dict), loc);
}

void GeneralScene::Sampler(const std::string &name, ParsedParameterVector params, FileLoc loc) {
    ParameterDictionary dict(std::move(params), graphicsState->colorSpace);

    VERIFY_OPTIONS("Sampler");
    sampler = GeneralSceneEntity(name, std::move(dict), loc);
}

void GeneralScene::Accelerator(const std::string &name, ParsedParameterVector params,
                               FileLoc loc) {
    ParameterDictionary dict(std::move(params), graphicsState->colorSpace);
    VERIFY_OPTIONS("Accelerator");
    accelerator = GeneralSceneEntity(name, std::move(dict), loc);
}

void GeneralScene::Integrator(const std::string &name, ParsedParameterVector params,
                              FileLoc loc) {
    ParameterDictionary dict(std::move(params), graphicsState->colorSpace);

    VERIFY_OPTIONS("Integrator");
    integrator = GeneralSceneEntity(name, std::move(dict), loc);
}

void GeneralScene::Camera(const std::string &name, ParsedParameterVector params,
                          FileLoc loc) {
    ParameterDictionary dict(std::move(params), graphicsState->colorSpace);

    VERIFY_OPTIONS("Camera");

    TransformSet cameraFromWorld = curTransform;
    TransformSet worldFromCamera = Inverse(curTransform);
    for (int i = 0; i < MaxTransforms; ++i) {
        Point3f pCamera = worldFromCamera[i](Point3f(0, 0, 0));
        cameraFromWorldT[i] = pbrt::Translate(-Vector3f(pCamera));
    }
    namedCoordinateSystems["camera"] = Inverse(cameraFromWorld);

    AnimatedTransform worldFromCameraAT(transformCache.Lookup(Inverse(cameraFromWorld[0] *
                                                                      Inverse(cameraFromWorldT[0]))),
                                        transformStartTime,
                                        transformCache.Lookup(Inverse(cameraFromWorld[1] *
                                                                      Inverse(cameraFromWorldT[1]))),
                                        transformEndTime);

    camera = CameraSceneEntity(name, std::move(dict), loc,
                               worldFromCameraAT,
                               graphicsState->currentOutsideMedium);
}

void GeneralScene::MakeNamedMedium(const std::string &name, ParsedParameterVector params,
                                   FileLoc loc) {
    VERIFY_INITIALIZED("MakeNamedMedium");
    WARN_IF_ANIMATED_TRANSFORM("MakeNamedMedium");
    ParameterDictionary dict(std::move(params), graphicsState->mediumAttributes,
                             graphicsState->colorSpace);

    if (media.find(name) != media.end())
        ErrorExit(&loc, "Named medium \"%s\" redefined.", name);

    AnimatedTransform worldFromMedium(transformCache.Lookup(GetCTM(0)),
                                      transformStartTime,
                                      transformCache.Lookup(GetCTM(1)),
                                      transformEndTime);

    media[name] = TransformedSceneEntity(name, std::move(dict),
                                                  loc, worldFromMedium);
}

void GeneralScene::MediumInterface(const std::string &insideName,
                                   const std::string &outsideName, FileLoc loc) {
    VERIFY_INITIALIZED("MediumInterface");
    graphicsState->currentInsideMedium = insideName;
    graphicsState->currentOutsideMedium = outsideName;
    haveScatteringMedia = true;
}

void GeneralScene::WorldBegin(FileLoc loc) {
    VERIFY_OPTIONS("WorldBegin");
    currentApiState = APIState::WorldBlock;
    for (int i = 0; i < MaxTransforms; ++i) curTransform[i] = pbrt::Transform();
    activeTransformBits = AllTransformsBits;
    namedCoordinateSystems["world"] = curTransform;
}

void GeneralScene::AttributeBegin(FileLoc loc) {
    VERIFY_WORLD("AttributeBegin");

    pushedGraphicsStates.push_back(*graphicsState);

    pushedTransforms.push_back(curTransform);
    pushedActiveTransformBits.push_back(activeTransformBits);

    pushStack.push_back(std::make_pair('a', loc));
}

void GeneralScene::AttributeEnd(FileLoc loc) {
    VERIFY_WORLD("AttributeEnd");
    if (pushedGraphicsStates.empty()) {
        Error(&loc, "Unmatched AttributeEnd encountered. Ignoring it.");
        return;
    }

    // NOTE: must keep the following consistent with code in ObjectEnd
    *graphicsState = std::move(pushedGraphicsStates.back());
    pushedGraphicsStates.pop_back();

    curTransform = pushedTransforms.back();
    pushedTransforms.pop_back();
    activeTransformBits = pushedActiveTransformBits.back();
    pushedActiveTransformBits.pop_back();

    if (pushStack.back().first == 't')
        ErrorExit(&loc, "Mismatched nesting: open TransformBegin from %s at AttributeEnd",
                  pushStack.back().second);
    else if (pushStack.back().first == 'o')
        ErrorExit(&loc, "Mismatched nesting: open ObjectBegin from %s at AttributeEnd",
                  pushStack.back().second);
    else
        CHECK_EQ(pushStack.back().first, 'a');
    pushStack.pop_back();
}

void GeneralScene::Attribute(const std::string &target, ParsedParameterVector attrib,
                             FileLoc loc) {
    VERIFY_INITIALIZED("Attribute");

    ParsedParameterVector *currentAttributes = nullptr;
    if (target == "shape") {
        currentAttributes = &graphicsState->shapeAttributes;
    } else if (target == "light") {
        currentAttributes = &graphicsState->lightAttributes;
    } else if (target == "material") {
        currentAttributes = &graphicsState->materialAttributes;
    } else if (target == "medium") {
        currentAttributes = &graphicsState->mediumAttributes;
    } else if (target == "texture") {
        currentAttributes = &graphicsState->textureAttributes;
    } else
        ErrorExit(&loc, "Unknown attribute target \"%s\". Must be \"shape\", \"light\", "
                  "\"material\", \"medium\", or \"texture\".", target);

    // Note that we hold on to the current color space and associate it
    // with the parameters...
    for (ParsedParameter *p : attrib) {
        p->mayBeUnused = true;
        p->colorSpace = graphicsState->colorSpace;
        currentAttributes->push_back(p);
    }
}

void GeneralScene::TransformBegin(FileLoc loc) {
    VERIFY_WORLD("TransformBegin");
    pushedTransforms.push_back(curTransform);
    pushedActiveTransformBits.push_back(activeTransformBits);
    pushStack.push_back(std::make_pair('t', loc));
}

void GeneralScene::TransformEnd(FileLoc loc) {
    VERIFY_WORLD("TransformEnd");
    if (pushedTransforms.empty()) {
        Error(&loc, "Unmatched TransformEnd encountered. Ignoring it.");
        return;
    }
    curTransform = pushedTransforms.back();
    pushedTransforms.pop_back();
    activeTransformBits = pushedActiveTransformBits.back();
    pushedActiveTransformBits.pop_back();

    if (pushStack.back().first == 'a')
        ErrorExit(&loc, "Mismatched nesting: open AttributeBegin from %s at TransformEnd",
                  pushStack.back().second);
    else if (pushStack.back().first == 'o')
        ErrorExit(&loc, "Mismatched nesting: open ObjectBegin from %s at TransformEnd",
                  pushStack.back().second);
    else
        CHECK_EQ(pushStack.back().first, 't');
    pushStack.pop_back();
}

void GeneralScene::Texture(const std::string &name, const std::string &type,
                           const std::string &texname, ParsedParameterVector params,
                           FileLoc loc) {
    VERIFY_WORLD("Texture");

    ParameterDictionary dict(std::move(params), graphicsState->textureAttributes,
                             graphicsState->colorSpace);

    AnimatedTransform worldFromTexture(transformCache.Lookup(GetCTM(0)),
                                       transformStartTime,
                                       transformCache.Lookup(GetCTM(1)),
                                       transformEndTime);

    if (type != "float" && type != "spectrum")
        ErrorExit(&loc, "%s: texture type unknown. Must be \"float\" or \"spectrum\".",
                  type);

    std::vector<std::pair<std::string, TextureSceneEntity>> &textures = (type == "float") ?
        floatTextures : spectrumTextures;

    for (const auto &tex : textures)
        if (tex.first == name)
            ErrorExit(&loc, "Redefining texture \"%s\".", name);

    textures.push_back(std::make_pair(name, TextureSceneEntity(texname, std::move(dict), loc,
                                                               worldFromTexture)));
}

void GeneralScene::Material(const std::string &name, ParsedParameterVector params,
                            FileLoc loc) {
    VERIFY_WORLD("Material");
    ParameterDictionary dict(std::move(params), graphicsState->materialAttributes,
                             graphicsState->colorSpace);
    materials.push_back(GeneralSceneEntity(name, std::move(dict), loc));
    graphicsState->currentMaterialIndex = materials.size() - 1;
    graphicsState->currentMaterialName.clear();
}

void GeneralScene::MakeNamedMaterial(const std::string &name, ParsedParameterVector params,
                                     FileLoc loc) {
    VERIFY_WORLD("MakeNamedMaterial");

    ParameterDictionary dict(std::move(params), graphicsState->materialAttributes,
                             graphicsState->colorSpace);

    if (namedMaterials.find(name) != namedMaterials.end())
        ErrorExit("%s: named material redefined.", name);

    namedMaterials[name] = GeneralSceneEntity("", std::move(dict), loc);
}

void GeneralScene::NamedMaterial(const std::string &name, FileLoc loc) {
    VERIFY_WORLD("NamedMaterial");
    graphicsState->currentMaterialName = name;
    graphicsState->currentMaterialIndex = -1;
}

void GeneralScene::LightSource(const std::string &name, ParsedParameterVector params,
                               FileLoc loc) {
    VERIFY_WORLD("LightSource");
    ParameterDictionary dict(std::move(params), graphicsState->lightAttributes,
                             graphicsState->colorSpace);
    AnimatedTransform worldFromLight(transformCache.Lookup(GetCTM(0)),
                                     transformStartTime,
                                     transformCache.Lookup(GetCTM(1)),
                                     transformEndTime);

    lights.push_back(LightSceneEntity(name, std::move(dict), loc,
                                               worldFromLight,
                                               graphicsState->currentOutsideMedium));
}

void GeneralScene::AreaLightSource(const std::string &name, ParsedParameterVector params,
                                   FileLoc loc) {
    VERIFY_WORLD("AreaLightSource");
    graphicsState->areaLightName = name;
    graphicsState->areaLightParams = ParameterDictionary(std::move(params), graphicsState->lightAttributes,
                                                         graphicsState->colorSpace);
    graphicsState->areaLightLoc = loc;
}

void GeneralScene::Shape(const std::string &name, ParsedParameterVector params,
                         FileLoc loc) {
    VERIFY_WORLD("Shape");

    ParameterDictionary dict(std::move(params), graphicsState->shapeAttributes,
                             graphicsState->colorSpace);

    int areaLightIndex = -1;
    if (!graphicsState->areaLightName.empty()) {
        areaLights.push_back(GeneralSceneEntity(graphicsState->areaLightName,
                                                graphicsState->areaLightParams,
                                                graphicsState->areaLightLoc));
        areaLightIndex = areaLights.size() - 1;
    }

    if (CTMIsAnimated()) {
        std::vector<AnimatedShapeSceneEntity> *as = &animatedShapes;
        if (currentInstance != nullptr) {
            if (!graphicsState->areaLightName.empty())
                Warning(&loc, "Area lights not supported with object instancing");
            as = &currentInstance->second;
        }

        AnimatedTransform worldFromShape(transformCache.Lookup(GetCTM(0)),
                                         transformStartTime,
                                         transformCache.Lookup(GetCTM(1)),
                                         transformEndTime);
        const class Transform *identity = transformCache.Lookup(pbrt::Transform());

        as->push_back(AnimatedShapeSceneEntity({name, std::move(dict), loc,
                                                worldFromShape, identity,
                                                graphicsState->reverseOrientation,
                                                graphicsState->currentMaterialIndex,
                                                graphicsState->currentMaterialName,
                                                areaLightIndex,
                                                graphicsState->currentInsideMedium,
                                                graphicsState->currentOutsideMedium}));
    } else {
        std::vector<ShapeSceneEntity> *s = &shapes;
        if (currentInstance != nullptr) {
            if (!graphicsState->areaLightName.empty())
                Warning(&loc, "Area lights not supported with object instancing");
            s = &currentInstance->first;
        }

        const class Transform *worldFromObject = transformCache.Lookup(GetCTM(0));
        const class Transform *objectFromWorld = transformCache.Lookup(Inverse(*worldFromObject));

        s->push_back(ShapeSceneEntity({name, std::move(dict), loc,
                                       worldFromObject, objectFromWorld,
                                       graphicsState->reverseOrientation,
                                       graphicsState->currentMaterialIndex,
                                       graphicsState->currentMaterialName,
                                       areaLightIndex,
                                       graphicsState->currentInsideMedium,
                                       graphicsState->currentOutsideMedium}));
    }
}

void GeneralScene::ReverseOrientation(FileLoc loc) {
    VERIFY_WORLD("ReverseOrientation");
    graphicsState->reverseOrientation = !graphicsState->reverseOrientation;
}

void GeneralScene::ObjectBegin(const std::string &name, FileLoc loc) {
    VERIFY_WORLD("ObjectBegin");
    pushedGraphicsStates.push_back(*graphicsState);
    pushedTransforms.push_back(curTransform);
    pushedActiveTransformBits.push_back(activeTransformBits);

    pushStack.push_back(std::make_pair('o', loc));

    // Set the shape name attribute using the instance name.
    ParsedParameterVector params;
    // TODO: should like the same arena that the parser is using for
    // parameter stuff here...
    ParsedParameter *p = sceneArena.Alloc<ParsedParameter>(&sceneArena, loc);
    p->type = "string";
    p->name = "name";
    p->AddString(name);
    Attribute("shape", std::move(params), loc);

    if (currentInstance != nullptr)
        ErrorExit(&loc, "ObjectBegin called inside of instance definition");
    if (instanceDefinitions.find(name) !=
        instanceDefinitions.end()) {
        ErrorExit(&loc, "%s: trying to redefine an object instance", name);
    }

    instanceDefinitions[name] = std::make_pair(std::vector<ShapeSceneEntity>(),
                                               std::vector<AnimatedShapeSceneEntity>());
    currentInstance = &instanceDefinitions[name];
}

STAT_COUNTER("Scene/Object instances created", nObjectInstancesCreated);

void GeneralScene::ObjectEnd(FileLoc loc) {
    VERIFY_WORLD("ObjectEnd");
    if (currentInstance == nullptr)
        ErrorExit(&loc, "ObjectEnd called outside of instance definition");
    currentInstance = nullptr;

    // NOTE: Must keep the following consistent with AttributeEnd
    *graphicsState = std::move(pushedGraphicsStates.back());
    pushedGraphicsStates.pop_back();

    curTransform = pushedTransforms.back();
    pushedTransforms.pop_back();
    activeTransformBits = pushedActiveTransformBits.back();
    pushedActiveTransformBits.pop_back();

    ++nObjectInstancesCreated;

    if (pushStack.back().first == 't')
        ErrorExit(&loc, "Mismatched nesting: open TransformBegin from %s at ObjectEnd",
                  pushStack.back().second);
    else if (pushStack.back().first == 'a')
        ErrorExit(&loc, "Mismatched nesting: open AttributeBegin from %s at ObjectEnd",
                  pushStack.back().second);
    else
        CHECK_EQ(pushStack.back().first, 'o');
    pushStack.pop_back();

}

STAT_COUNTER("Scene/Object instances used", nObjectInstancesUsed);

void GeneralScene::ObjectInstance(const std::string &name, FileLoc loc) {
    VERIFY_WORLD("ObjectInstance");

    if (currentInstance != nullptr)
        ErrorExit(&loc, "ObjectInstance can't be called inside instance definition");

    TransformSet worldFromCameraT = Inverse(cameraFromWorldT);

    if (CTMIsAnimated()) {
        const class Transform *WorldFromInstance[2] = {
            transformCache.Lookup(GetCTM(0) * worldFromCameraT[0]),
            transformCache.Lookup(GetCTM(1) * worldFromCameraT[1])
        };
        AnimatedTransform animatedWorldFromInstance(
            WorldFromInstance[0], transformStartTime,
            WorldFromInstance[1], transformEndTime);

        instances.push_back(InstanceSceneEntity(name, loc,
                                                animatedWorldFromInstance,
                                                nullptr));
    } else {
        const class Transform *worldFromInstance =
            transformCache.Lookup(GetCTM(0) * worldFromCameraT[0]);

        instances.push_back(InstanceSceneEntity(name, loc,
                                                AnimatedTransform(),
                                                worldFromInstance));
    }
}

void GeneralScene::WorldEnd(FileLoc loc) {
    VERIFY_WORLD("WorldEnd");
    // Ensure there are no pushed graphics states
    while (!pushedGraphicsStates.empty()) {
        Warning(&loc, "Missing end to AttributeBegin");
        pushedGraphicsStates.pop_back();
        pushedTransforms.pop_back();
    }
    while (!pushedTransforms.empty()) {
        Warning(&loc, "Missing end to TransformBegin");
        pushedTransforms.pop_back();
    }

    CHECK(PbrtOptions.renderFunction != nullptr);
    PbrtOptions.renderFunction(*this);

    ForEachThread(ReportThreadStats);

    if (PbrtOptions.recordPixelStatistics)
        StatsWritePixelImages();

    if (!PbrtOptions.quiet) {
        PrintStats(stdout);
        if (PbrtOptions.profile) {
            ReportProfilerResults(stdout);
            ClearProfiler();
        }
        ClearStats();
    }
    if (PrintCheckRare(stdout))
        ErrorExit("CHECK_RARE failures");

    LOG_VERBOSE("Memory used after post-render cleanup: %s", GetCurrentRSS());
}

void GeneralScene::CreateMaterials(/*const*/std::map<std::string, FloatTextureHandle> &floatTextures,
                                   /*const*/std::map<std::string, SpectrumTextureHandle> &spectrumTextures,
                                   Allocator alloc,
                                   std::map<std::string, MaterialHandle> *namedMaterialsOut,
                                   std::vector<MaterialHandle> *materialsOut) const {
    // Named materials
    for (const auto &nm : namedMaterials) {
        const std::string &name = nm.first;
        const GeneralSceneEntity &mtl = nm.second;
        if (namedMaterialsOut->find(name) != namedMaterialsOut->end())
            ErrorExit(&mtl.loc, "%s: named material redefined.", name);

        std::string type = mtl.parameters.GetOneString("type", "");
        if (type.empty())
            ErrorExit(&mtl.loc, "%s: \"string type\" not provided in named material's "
                      "parameters.", name);
        TextureParameterDictionary texDict(&mtl.parameters, &floatTextures, &spectrumTextures);
        MaterialHandle m = MaterialHandle::Create(type, texDict, *namedMaterialsOut,
                                                  alloc, mtl.loc);
        (*namedMaterialsOut)[name] = m;
    }

    // Regular materials
    materialsOut->reserve(materials.size());
    for (const auto &mtl : materials) {
        TextureParameterDictionary texDict(&mtl.parameters, &floatTextures, &spectrumTextures);
        MaterialHandle m = MaterialHandle::Create(mtl.name, texDict, *namedMaterialsOut,
                                                  alloc, mtl.loc);
        materialsOut->push_back(m);
    }
}

///////////////////////////////////////////////////////////////////////////
// FormattingScene

void FormattingScene::Option(const std::string &name, const std::string &value, FileLoc loc) {
    std::string nName = normalizeArg(name);
    if (nName == "msereferenceimage" || nName == "msereferenceout")
        Printf("%sOption \"%s\" \"%s\"\n", indent(), name, value);
    else
        Printf("%sOption \"%s\" %s\n", indent(), name, value);
}

void FormattingScene::Identity(FileLoc loc) {
    Printf("%sIdentity\n", indent());
}

void FormattingScene::Translate(Float dx, Float dy, Float dz, FileLoc loc) {
    Printf("%sTranslate %f %f %f\n", indent(), dx, dy, dz);
}

void FormattingScene::Rotate(Float angle, Float ax, Float ay, Float az, FileLoc loc) {
    Printf("%sRotate %f %f %f %f\n", indent(), angle, ax, ay, az);
}

void FormattingScene::Scale(Float sx, Float sy, Float sz, FileLoc loc) {
    Printf("%sScale %f %f %f\n", indent(), sx, sy, sz);
}

void FormattingScene::LookAt(Float ex, Float ey, Float ez, Float lx, Float ly, Float lz,
                             Float ux, Float uy, Float uz, FileLoc loc) {
    Printf("%sLookAt %f %f %f\n%s    %f %f %f\n%s    %f %f %f\n",
           indent(), ex, ey, ez, indent(), lx, ly, lz,
           indent(), ux, uy, uz);
}

void FormattingScene::ConcatTransform(Float transform[16], FileLoc loc) {
    Printf("%sConcatTransform [ ", indent());
    for (int i = 0; i < 16; ++i) Printf("%f ", transform[i]);
    Printf(" ]\n");
}

void FormattingScene::Transform(Float transform[16], FileLoc loc) {
    Printf("%sTransform [ ", indent());
    for (int i = 0; i < 16; ++i) Printf("%f ", transform[i]);
    Printf(" ]\n");
}

void FormattingScene::CoordinateSystem(const std::string &name, FileLoc loc) {
    Printf("%sCoordinateSystem \"%s\"\n", indent(), name);
}

void FormattingScene::CoordSysTransform(const std::string &name, FileLoc loc) {
    Printf("%sCoordSysTransform \"%s\"\n", indent(), name);
}

void FormattingScene::ActiveTransformAll(FileLoc loc) {
    Printf("%sActiveTransform All\n", indent());
}

void FormattingScene::ActiveTransformEndTime(FileLoc loc) {
    Printf("%sActiveTransform EndTime\n", indent());
}

void FormattingScene::ActiveTransformStartTime(FileLoc loc) {
    Printf("%sActiveTransform StartTime\n", indent());
}

void FormattingScene::TransformTimes(Float start, Float end, FileLoc loc) {
    Printf("%sTransformTimes %f %f\n", indent(), start, end);
}

void FormattingScene::ColorSpace(const std::string &n, FileLoc loc) {
    Printf("%sColorSpace \"%s\"\n", indent(), n);
}

void FormattingScene::PixelFilter(const std::string &name, ParsedParameterVector params,
                                  FileLoc loc) {
    ParameterDictionary dict(std::move(params), RGBColorSpace::sRGB);

    std::string extra;
    if (upgrade) {
        std::vector<Float> xr = dict.GetFloatArray("xwidth");
        if (xr.size() == 1) {
            dict.RemoveFloat("xwidth");
            extra += StringPrintf("%s\"float xradius\" [ %f ]\n", indent(1), xr[0]);
        }
        std::vector<Float> yr = dict.GetFloatArray("ywidth");
        if (yr.size() == 1) {
            dict.RemoveFloat("ywidth");
            extra += StringPrintf("%s\"float yradius\" [ %f ]\n", indent(1), yr[0]);
        }

        if (name == "gaussian") {
            std::vector<Float> alpha = dict.GetFloatArray("alpha");
            if (alpha.size() == 1) {
                dict.RemoveFloat("alpha");
                extra += StringPrintf("%s\"float sigma\" [ %f ]\n", indent(1),
                                      1 / std::sqrt(2 * alpha[0]));
            }
        }
    }

    Printf("%sPixelFilter \"%s\"\n", indent(), name);
    std::cout << extra << dict.ToParameterList(catIndentCount);
}

void FormattingScene::Film(const std::string &type, ParsedParameterVector params,
                           FileLoc loc) {
    ParameterDictionary dict(std::move(params), RGBColorSpace::sRGB);

    if (upgrade && type == "image")
        Printf("%sFilm \"rgb\"\n", indent());
    else
        Printf("%sFilm \"%s\"\n", indent(), type);
    std::cout << dict.ToParameterList(catIndentCount);
}

void FormattingScene::Sampler(const std::string &name, ParsedParameterVector params,
                              FileLoc loc) {
    ParameterDictionary dict(std::move(params), RGBColorSpace::sRGB);

    if (upgrade) {
        if (name == "lowdiscrepancy" || name == "02sequence")
            Printf("%sSampler \"paddedsobol\"\n", indent());
        else if (name == "maxmindist")
            Printf("%sSampler \"pmj02bn\"\n", indent());
        else
            Printf("%sSampler \"%s\"\n", indent(), name);
    } else
        Printf("%sSampler \"%s\"\n", indent(), name);
    std::cout << dict.ToParameterList(catIndentCount);
}

void FormattingScene::Accelerator(const std::string &name, ParsedParameterVector params,
                                  FileLoc loc) {
    ParameterDictionary dict(std::move(params), RGBColorSpace::sRGB);

    Printf("%sAccelerator \"%s\"\n%s", indent(), name,
           dict.ToParameterList(catIndentCount));
}

void FormattingScene::Integrator(const std::string &name, ParsedParameterVector params,
                                 FileLoc loc) {
    ParameterDictionary dict(std::move(params), RGBColorSpace::sRGB);

    std::string extra;
    if (upgrade) {
        if (name == "sppm") {
            dict.RemoveInt("imagewritefrequency");

            std::vector<int> iterations = dict.GetIntArray("numiterations");
            if (!iterations.empty()) {
                dict.RemoveInt("numiterations");
                extra += indent(1) + StringPrintf("\"integer iterations\" [ %d ]\n", iterations[0]);
            }
        }
        std::string lss = dict.GetOneString("lightsamplestrategy", "");
        if (lss == "spatial") {
            dict.RemoveString("lightsamplestrategy");
            extra += indent(1) + "\"string lightsamplestrategy\" \"bvh\"\n";
        }
    }

    if (upgrade && name == "directlighting") {
        Printf("%sIntegrator \"path\"\n", indent());
        extra += indent(1) + "\"integer maxdepth\" [ 1 ]\n";
    } else
        Printf("%sIntegrator \"%s\"\n", indent(), name);
    std::cout << extra << dict.ToParameterList(catIndentCount);
}

void FormattingScene::Camera(const std::string &name, ParsedParameterVector params,
                             FileLoc loc) {
    ParameterDictionary dict(std::move(params), RGBColorSpace::sRGB);

    if (upgrade && name == "environment")
        Printf("%sCamera \"spherical\" \"string mapping\" \"equirect\"\n", indent());
    else
        Printf("%sCamera \"%s\"\n", indent(), name);
    if (upgrade && name == "realistic")
        dict.RemoveBool("simpleweighting");

    std::cout << dict.ToParameterList(catIndentCount);
}

void FormattingScene::MakeNamedMedium(const std::string &name, ParsedParameterVector params,
                                      FileLoc loc) {
    ParameterDictionary dict(params, RGBColorSpace::sRGB);
    Printf("%sMakeNamedMedium \"%s\"\n%s\n", indent(), name,
           dict.ToParameterList(catIndentCount));
}

void FormattingScene::MediumInterface(const std::string &insideName,
                                      const std::string &outsideName, FileLoc loc) {
    Printf("%sMediumInterface \"%s\" \"%s\"\n", indent(), insideName, outsideName);
}

void FormattingScene::WorldBegin(FileLoc loc) {
    Printf("\n\nWorldBegin\n\n");
}

void FormattingScene::AttributeBegin(FileLoc loc) {
    Printf("\n%sAttributeBegin\n", indent());
    catIndentCount += 4;
}

void FormattingScene::AttributeEnd(FileLoc loc) {
    catIndentCount -= 4;
    Printf("%sAttributeEnd\n", indent());
}

void FormattingScene::Attribute(const std::string &target, ParsedParameterVector params,
                                FileLoc loc) {
    ParameterDictionary dict(params, RGBColorSpace::sRGB);
    Printf("%sAttribute \"%s\" ", indent(), target);
    if (params.size() == 1)
        // just one; put it on the same line
        std::cout << dict.ToParameterList(0) << '\n';
    else
        std::cout << '\n' << dict.ToParameterList(catIndentCount);
}

void FormattingScene::TransformBegin(FileLoc loc) {
    Printf("%sTransformBegin\n", indent());
    catIndentCount += 4;
}

void FormattingScene::TransformEnd(FileLoc loc) {
    catIndentCount -= 4;
    Printf("%sTransformEnd\n", indent());
}

void FormattingScene::Texture(const std::string &name, const std::string &type,
                              const std::string &texname, ParsedParameterVector params,
                              FileLoc loc) {
    if (upgrade && texname == "scale") {
        // This is easier to do in the raw ParsedParameterVector...
        if (type == "float") {
            for (ParsedParameter *p : params) {
                if (p->name == "tex1")
                    p->name = "tex";
                if (p->name == "tex2")
                    p->name = "scale";
            }
        } else {
            // more subtle: rename one of them as float, but need one of them
            // to be an RGB and spectrally constant...
            bool foundRGB = false, foundTexture = false;
            for (ParsedParameter *p : params) {
                if (p->name != "tex1" && p->name != "tex2")
                    continue;

                if (p->type == "rgb") {
                    if (foundRGB)
                        ErrorExit(&p->loc, "Two \"rgb\" textures found for \"scale\" "
                                  "texture \"%s\". Please manually edit the file to "
                                  "upgrade.", name);
                    if (p->numbers.size() != 3)
                        ErrorExit(&p->loc, "Didn't find 3 values for \"rgb\" \"%s\".",
                                  p->name);
                    if (p->numbers[0] != p->numbers[1] || p->numbers[1] != p->numbers[2])
                        ErrorExit(&p->loc, "Non-constant \"rgb\" value found for "
                                  "\"scale\" texture parameter \"%s\". Please manually "
                                  "edit the file to upgrade.", p->name);

                    foundRGB = true;
                    p->type = "float";
                    p->name = "scale";
                    p->numbers.resize(1);
                } else {
                    if (foundTexture)
                        ErrorExit(&p->loc, "Two textures found for \"scale\" "
                                  "texture \"%s\". Please manually edit the file to "
                                  "upgrade.", name);
                    p->name = "tex";
                    foundTexture = true;
                }
            }
        }
    }

    ParameterDictionary dict(params, RGBColorSpace::sRGB);

    std::string extra;
    if (upgrade) {
        if (texname == "imagemap") {
            std::vector<uint8_t> tri = dict.GetBoolArray("trilinear");
            if (tri.size() == 1) {
                dict.RemoveBool("trilinear");
                extra += indent(1) + "\"string filter\" ";
                extra += tri[0] != 0u ? "\"trilinear\"\n" : "\"bilinear\"\n";
            }
        }

        if (texname == "imagemap" || texname == "ptex") {
            std::string n = dict.GetOneString("filename", "");
            if (!n.empty()) {
                dict.RemoveString("filename");
                extra += indent(1) + "\"string imagefile\" \"" + n + "\"\n";
            }

            Float gamma = dict.GetOneFloat("gamma", 0);
            if (gamma != 0) {
                dict.RemoveFloat("gamma");
                extra += indent(1) + StringPrintf("\"string encoding \" \"gamma %f\" ", gamma);
            } else {
                std::vector<uint8_t> gamma = dict.GetBoolArray("gamma");
                if (gamma.size() == 1) {
                    dict.RemoveBool("gamma");
                    extra += indent(1) + "\"string encoding\" ";
                    extra += gamma[0] != 0u ? "\"sRGB\"\n" : "\"linear\"\n";
                }
            }
        }
    }

    if (upgrade && type == "color")
        Printf("%sTexture \"%s\" \"spectrum\" \"%s\"\n", indent(),
               name, texname);
    else
        Printf("%sTexture \"%s\" \"%s\" \"%s\"\n", indent(),
               name, type, texname);
    std::cout << extra << dict.ToParameterList(catIndentCount);
}

static std::string upgradeMaterialIndex(
    const FormattingScene &scene, const std::string &name, ParameterDictionary *dict,
    FileLoc loc) {
    if (name != "glass" && name != "uber")
        return "";

    std::string tex = dict->GetTexture("index");
    if (!tex.empty()) {
        if (!dict->GetTexture("eta").empty())
            ErrorExit(&loc, R"(Material "%s" has both "index" and "eta" parameters.)",
                      name);

        dict->RemoveTexture("index");
        return scene.indent(1) + StringPrintf("\"texture eta\" \"%s\"\n", tex);
    } else {
        auto index = dict->GetFloatArray("index");
        if (index.empty()) return "";
        if (index.size() != 1)
            ErrorExit(&loc, "Multiple values provided for \"index\" parameter.");
        if (!dict->GetFloatArray("eta").empty())
            ErrorExit(&loc, R"(Material "%s" has both "index" and "eta" parameters.)",
                      name);

        Float value = index[0];
        dict->RemoveFloat("index");
        return scene.indent(1) + StringPrintf("\"float eta\" [ %f ]\n", value);
    }
}

static std::string upgradeMaterialBumpmap(const FormattingScene &scene,
                                          ParameterDictionary *dict) {
    std::string bump = dict->GetTexture("bumpmap");
    if (bump.empty())
        return "";

    dict->RemoveTexture("bumpmap");
    return scene.indent(1) + StringPrintf("\"texture displacement\" \"%s\"\n", bump);
}

static void upgradeUberOpacity(ParameterDictionary *dict, FileLoc loc) {
    if (!dict->GetTexture("opacity").empty())
        ErrorExit(&loc, "Non-opaque \"opacity\" in \"uber\" material not supported "
                  "in pbrt-v4. Please edit the file manually.");

    if (dict->GetSpectrumArray("opacity", SpectrumType::Reflectance, {}).empty())
        return;

    pstd::optional<RGB> opacity = dict->GetOneRGB("opacity");
    if (opacity && opacity->r == 1 && opacity->g == 1 && opacity->b == 1) {
        dict->RemoveSpectrum("opacity");
        return;
    }

    ErrorExit(&loc, "A non-opaque \"opacity\" in the \"uber\" material is not supported "
              "in pbrt-v4. Please edit the file manually.");
}

static std::string upgradeMaterial(const FormattingScene &scene, std::string *name,
                                   ParameterDictionary *dict, FileLoc loc) {
    std::string extra = upgradeMaterialIndex(scene, *name, dict, loc);
    extra += upgradeMaterialBumpmap(scene, dict);

    auto removeParamSilentIfConstant = [&](const char *paramName, Float value) {
        pstd::optional<RGB> rgb = dict->GetOneRGB(paramName);
        bool matches = (rgb && rgb->r == value && rgb->g == value && rgb->b == value);

        if (!matches && !dict->GetSpectrumArray(paramName, SpectrumType::Reflectance, {}).empty())
            Warning(&loc, "Parameter is being removed when converting "
                    "to \"%s\" material: %s", *name,
                    dict->ToParameterDefinition(paramName));
        dict->RemoveSpectrum(paramName);
        dict->RemoveTexture(paramName);
        return matches;
    };

    if (*name == "uber") {
        *name = "coateddiffuse";
        if (removeParamSilentIfConstant("Ks", 0)) {
            *name = "diffuse";
            dict->RemoveFloat("eta");
            dict->RemoveFloat("roughness");
        }
        removeParamSilentIfConstant("Kr", 0);
        removeParamSilentIfConstant("Kt", 0);
        dict->RenameParameter("Kd", "reflectance");
        upgradeUberOpacity(dict, loc);
    } else if (*name == "mix") {
        pstd::optional<RGB> rgb = dict->GetOneRGB("amount");
        if (rgb) {
            if (rgb->r == rgb->g && rgb->g == rgb->b)
                extra += scene.indent(1) + StringPrintf("\"float amount\" [ %f ]\n", rgb->r);
            else {
                Float avg = (rgb->r + rgb->g + rgb->b) / 3;
                Warning(&loc, "Changing RGB \"amount\" (%f, %f, %f) to scalar average %f",
                        rgb->r, rgb->g, rgb->b, avg);
                extra += scene.indent(1) + StringPrintf("\"float amount\" [ %f ]\n", avg);
            }
        } else if (!dict->GetSpectrumArray("amount", SpectrumType::General, {}).empty() ||
                   !dict->GetTexture("amount").empty())
            Error(&loc, "Unable to update non-RGB spectrum \"amount\" to a scalar: %s",
                  dict->ToParameterDefinition("amount"));

        dict->RemoveSpectrum("amount");
    } else if (*name == "substrate") {
        *name = "coateddiffuse";
        removeParamSilentIfConstant("Ks", 1);
        dict->RenameParameter("Kd", "reflectance");
    } else if (*name == "glass") {
        *name = "dielectric";
        removeParamSilentIfConstant("Kr", 1);
        removeParamSilentIfConstant("Kt", 1);
    } else if (*name == "plastic") {
        *name = "coateddiffuse";
        if (removeParamSilentIfConstant("Ks", 0)) {
            *name = "diffuse";
            dict->RemoveFloat("roughness");
            dict->RemoveFloat("eta");
        }
        dict->RenameParameter("Kd", "reflectance");
    }
    else if (*name == "fourier")
        Warning(&loc, "\"fourier\" material is no longer supported. (But there is \"measured\"!)");
    else if (*name == "kdsubsurface") {
        *name = "subsurface";
        dict->RenameParameter("Kd", "reflectance");
    } else if (*name == "matte") {
        *name = "diffuse";
        dict->RenameParameter("Kd", "reflectance");
    } else if (*name == "metal") {
        *name = "conductor";
        removeParamSilentIfConstant("Kr", 1);
    } else if (*name == "translucent") {
        *name = "diffusetransmission";

        dict->RenameParameter("Kd", "transmittance");

        removeParamSilentIfConstant("reflect", 0);
        removeParamSilentIfConstant("transmit", 1);

        removeParamSilentIfConstant("Ks", 0);
        dict->RemoveFloat("roughness");
    } else if (*name == "mirror") {
        *name = "conductor";
        extra += scene.indent(1) + "\"float roughness\" [ 0 ]\n";
        extra += scene.indent(1) + "\"spectrum eta\" [ \"metal-Ag-eta\" ]\n";
        extra += scene.indent(1) + "\"spectrum k\" [ \"metal-Ag-k\" ]\n";

        removeParamSilentIfConstant("Kr", 0);
    }

    return extra;
}

void FormattingScene::Material(const std::string &name, ParsedParameterVector params,
                               FileLoc loc) {
    ParameterDictionary dict(params, RGBColorSpace::sRGB);
    std::string extra;
    std::string newName = name;
    if (upgrade)
        extra = upgradeMaterial(*this, &newName, &dict, loc);

    Printf("%sMaterial \"%s\"\n", indent(), newName);
    std::cout << extra << dict.ToParameterList(catIndentCount);
}

void FormattingScene::MakeNamedMaterial(const std::string &name, ParsedParameterVector params,
                                        FileLoc loc) {
    ParameterDictionary dict(params, RGBColorSpace::sRGB);
    Printf("%sMakeNamedMaterial \"%s\"\n", indent(), name);
    std::string extra;
    if (upgrade) {
        std::string matName = dict.GetOneString("type", "");
        extra = upgradeMaterial(*this, &matName, &dict, loc);
        dict.RemoveString("type");
        extra = indent(1) + StringPrintf("\"string type\" [ \"%s\" ]\n", matName) + extra;
    }
    std::cout << extra << dict.ToParameterList(catIndentCount);
}

void FormattingScene::NamedMaterial(const std::string &name, FileLoc loc) {
    Printf("%sNamedMaterial \"%s\"\n", indent(), name);
}

static bool upgradeRGBToScale(ParameterDictionary *dict, const char *name, Float *totalScale) {
    std::vector<SpectrumHandle> s = dict->GetSpectrumArray(name, SpectrumType::General, {});
    if (s.empty())
        return true;

    pstd::optional<RGB> rgb = dict->GetOneRGB(name);
    if (!rgb || rgb->r != rgb->g || rgb->g != rgb->b)
        return false;

    *totalScale *= rgb->r;
    dict->RemoveSpectrum(name);
    return true;
}

static std::string upgradeBlackbody(const FormattingScene &scene,
                                    ParameterDictionary *dict, Float *totalScale) {
    std::string extra;
    for (const char *name : { "L", "I" }) {
        std::vector<SpectrumHandle> spec = dict->GetSpectrumArray(name, SpectrumType::General, {});
        if (spec.empty()) continue;

        BlackbodySpectrum *bb = spec[0].CastOrNullptr<BlackbodySpectrum>();
        if (bb == nullptr) continue;

        if (spec.size() == 1)
            // Already been upgraded
            continue;

        // In pbrt-v3, the second parameter value is the scale factor. Pull it out
        // and incorporate it in the light's "scale" parameter value.
        const BlackbodySpectrum *bscale = spec[1].CastOrNullptr<BlackbodySpectrum>();
        Float scale = dict->GetOneFloat("scale", 1);
        dict->RemoveFloat("scale");
        *totalScale *= scale * bscale->T;

        dict->RemoveSpectrum(name);
        extra += scene.indent(1) + StringPrintf("\"blackbody %s\" [ %f ]\n", name, bb->T);
    }

    return extra;
}

static std::string upgradeMapname(const FormattingScene &scene,
                                  ParameterDictionary *dict) {
    std::string n = dict->GetOneString("mapname", "");
    if (n.empty())
        return "";

    dict->RemoveString("mapname");
    return scene.indent(1) + StringPrintf("\"string imagefile\" \"%s\"\n", n);
}

void FormattingScene::LightSource(const std::string &name, ParsedParameterVector params,
                                  FileLoc loc) {
    ParameterDictionary dict(params, RGBColorSpace::sRGB);

    Printf("%sLightSource \"%s\"\n", indent(), name);

    std::string extra;
    if (upgrade) {
        Float totalScale = 1;
        if (!upgradeRGBToScale(&dict, "scale", &totalScale))
            ErrorExit(dict.loc("scale"),
                      "In pbrt-v4, \"scale\" is now a \"float\" parameter to light sources. "
                      "Please modify your scene file manually.");
        extra += upgradeBlackbody(*this, &dict, &totalScale);
        dict.RemoveInt("nsamples");

        if (dict.GetOneString("mapname", "").empty() == false) {
            if (name == "infinite" && !upgradeRGBToScale(&dict, "L", &totalScale))
                ErrorExit(dict.loc("L"),
                          "Non-constant \"L\" is no longer supported with \"mapname\" for "
                          "the \"infinite\" light source. Please upgrade your scene "
                          "file manually.");
        } else if (name == "projection" && !upgradeRGBToScale(&dict, "I", &totalScale))
            ErrorExit(dict.loc("I"),
                      "\"I\" is no longer supported with \"mapname\" for "
                      "the \"projection\" light source. Please upgrade your scene "
                      "file manually.");

        // Do this after we've handled infinite "L" with a map, since
        // it removes the "mapname" parameter from the dictionary.
        extra += upgradeMapname(*this, &dict);

        if (totalScale != 1) {
            totalScale *= dict.GetOneFloat("scale", 1.f);
            dict.RemoveFloat("scale");
            Printf("%s\"float scale\" [%f]\n", indent(1), totalScale);
        }
    }

    std::cout << extra << dict.ToParameterList(catIndentCount);
}

void FormattingScene::AreaLightSource(const std::string &name, ParsedParameterVector params,
                                      FileLoc loc) {
    ParameterDictionary dict(params, RGBColorSpace::sRGB);
    std::string extra;
    Float totalScale = 1;
    if (upgrade) {
        if (!upgradeRGBToScale(&dict, "scale", &totalScale))
            ErrorExit(dict.loc("scale"),
                      "In pbrt-v4, \"scale\" is now a \"float\" parameter to light sources. "
                      "Please modify your scene file manually.");

        extra += upgradeBlackbody(*this, &dict, &totalScale);
        if (name == "area")
            Printf("%sAreaLightSource \"diffuse\"\n", indent());
        else
            Printf("%sAreaLightSource \"%s\"\n", indent(), name);
        dict.RemoveInt("nsamples");
    } else
        Printf("%sAreaLightSource \"%s\"\n", indent(), name);

    if (totalScale != 1)
        Printf("%s\"float scale\" [%f]", indent(1), totalScale);
    std::cout << extra << dict.ToParameterList(catIndentCount);
}

static std::string upgradeTriMeshUVs(const FormattingScene &scene,
                                     ParameterDictionary *dict) {
    std::vector<Point2f> uv = dict->GetPoint2fArray("st");
    if (!uv.empty())
        dict->RemovePoint2f("st");
    else {
        auto upgradeFloatArray =
            [&](const char *name) {
                std::vector<Float> fuv = dict->GetFloatArray(name);
                if (fuv.empty()) return;

                std::vector<Point2f> tempUVs;
                tempUVs.reserve(fuv.size() / 2);
                for (size_t i = 0; i < fuv.size() / 2; ++i)
                    tempUVs.push_back(Point2f(fuv[2 * i], fuv[2 * i + 1]));
                dict->RemoveFloat(name);
                uv = tempUVs;
            };
        upgradeFloatArray("uv");
        upgradeFloatArray("st");
    }

    if (uv.empty())
        return "";

    std::string s = scene.indent(1) + "\"point2 uv\" [ ";
    for (size_t i = 0; i < uv.size(); ++i) {
        s += StringPrintf("%f %f ", uv[i][0], uv[i][1]);
        if ((i + 1) % 4 == 0) {
            s += "\n";
            s += scene.indent(2);
        }
    }
    s += "]\n";
    return s;
}

void FormattingScene::Shape(const std::string &name, ParsedParameterVector params,
                            FileLoc loc) {
    ParameterDictionary dict(params, RGBColorSpace::sRGB);

    if (toPly && name == "trianglemesh") {
        std::vector<int> vi = dict.GetIntArray("indices");

        if (vi.size() < 500) {
            // It's a small mesh; don't bother with a PLY file after all.
            Printf("%sShape \"%s\"\n", indent(), name);
            std::cout << dict.ToParameterList(catIndentCount);
        } else {
            static int count = 1;
            const char *plyPrefix =
                getenv("PLY_PREFIX") != nullptr ? getenv("PLY_PREFIX") : "mesh";
            std::string fn = StringPrintf("%s_%05d.ply", plyPrefix, count++);

            std::vector<int> vi = dict.GetIntArray("indices");
            std::vector<Point3f> P = dict.GetPoint3fArray("P");
            std::vector<Point2f> uvs = dict.GetPoint2fArray("uv");
            std::vector<Normal3f> N = dict.GetNormal3fArray("N");
            std::vector<Vector3f> S = dict.GetVector3fArray("S");
            std::vector<int> faceIndices = dict.GetIntArray("faceIndices");
            if (!faceIndices.empty()) CHECK_EQ(faceIndices.size(), vi.size() / 3);

            if (!WritePlyFile(fn, vi, P, S, N, uvs, faceIndices))
                Error(&loc, "Unable to write PLY file \"%s\"", fn);

            dict.RemoveInt("indices");
            dict.RemovePoint3f("P");
            dict.RemovePoint2f("uv");
            dict.RemoveNormal3f("N");
            dict.RemoveVector3f("S");
            dict.RemoveInt("faceIndices");

            Printf("%sShape \"plymesh\" \"string filename\" \"%s\"\n",
                   indent(), fn);
            std::cout << dict.ToParameterList(catIndentCount);
        }
        return;
    }

    Printf("%sShape \"%s\"\n", indent(), name);

    if (upgrade) {
        if (name == "trianglemesh") {
            // Remove indices if they're [0 1 2] and we have a single triangle
            auto indices = dict.GetIntArray("indices");
            if (indices.size() == 3 && dict.GetPoint3fArray("P").size() == 3 &&
                indices[0] == 0 && indices[1] == 1 && indices[2] == 2)
                dict.RemoveInt("indices");
        }

        if (name == "bilinearmesh") {
            // Remove indices if they're [0 1 2 3] and we have a single blp
            auto indices = dict.GetIntArray("indices");
            if (indices.size() == 4 && dict.GetPoint3fArray("P").size() == 4 &&
                indices[0] == 0 && indices[1] == 1 && indices[2] == 2 &&
                indices[3] == 3)
                dict.RemoveInt("indices");
        }

        if (name == "loopsubdiv") {
            auto levels = dict.GetIntArray("nlevels");
            if (!levels.empty()) {
                Printf("%s\"integer levels\" [ %d ]\n", indent(1), levels[0]);
                dict.RemoveInt("nlevels");
            }
        }
        if (name == "trianglemesh" || name == "plymesh") {
            dict.RemoveBool("discarddegenerateUVs");
        }

        if (name == "plymesh") {
            std::string n = dict.GetOneString("filename", "");
            if (!n.empty()) {
                dict.RemoveString("filename");
                Printf("%s\"string plyfile\" \"%s\"\n", indent(1), n);
            }
        }

        if (name == "trianglemesh") {
            std::string extra = upgradeTriMeshUVs(*this, &dict);
            std::cout << extra;
        }

        //upgradeMaterialIndex(graphicsState->currentMaterial->name, &dict, loc);
        upgradeMaterialBumpmap(*this, &dict);
        dict.RenameParameter("Kd", "reflectance");
    }

    std::cout << dict.ToParameterList(catIndentCount);
}

void FormattingScene::ReverseOrientation(FileLoc loc) {
    Printf("%sReverseOrientation\n", indent());
}

void FormattingScene::ObjectBegin(const std::string &name, FileLoc loc) {
    Printf("%sObjectBegin \"%s\"\n", indent(), name);
}

void FormattingScene::ObjectEnd(FileLoc loc) {
    Printf("%sObjectEnd\n", indent());
}

void FormattingScene::ObjectInstance(const std::string &name, FileLoc loc) {
    Printf("%sObjectInstance \"%s\"\n", indent(), name);
}

void FormattingScene::WorldEnd(FileLoc loc) {
    Printf("%sWorldEnd\n", indent());
}


} // namespace pbrt
