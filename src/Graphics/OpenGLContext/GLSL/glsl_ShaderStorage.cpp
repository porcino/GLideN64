#include <fstream>
#include <assert.h>
#include <cstdlib>
#include <algorithm>
#include <iomanip>

#include <Graphics/CombinerProgram.h>
#include <Graphics/Context.h>
#include <Graphics/OpenGLContext/opengl_Utils.h>
#include <Types.h>
#include <Log.h>
#include <RSP.h>
#include <PluginAPI.h>
#include <Combiner.h>
#include <DisplayLoadProgress.h>
#include <osal_files.h>
#include <Config.h>
#include "glsl_Utils.h"
#include "glsl_ShaderStorage.h"
#include "glsl_CombinerProgramImpl.h"
#include "glsl_CombinerProgramUniformFactoryAccurate.h"
#include "glsl_CombinerProgramUniformFactoryFast.h"

using namespace glsl;

#define SHADER_STORAGE_FOLDER_NAME "shaders"

static
std::string getStorageFileName(const opengl::GLInfo & _glinfo, const char * _fileExtension)
{
	class SetLocale
	{
	public:
		SetLocale() : m_locale(setlocale(LC_CTYPE, NULL)) { setlocale(LC_CTYPE, ""); }
		~SetLocale() { setlocale(LC_CTYPE, m_locale.c_str()); }
	private:
		std::string m_locale;
	} setLocale;

	wchar_t strCacheFolderPath[PLUGIN_PATH_SIZE];
	api().GetUserCachePath(strCacheFolderPath);

	// Convert wchar string to multibyte string
	// Use large enough buffer to hold multibyte conversion of wchar string
	char strCacheFolderPathChar[PLUGIN_PATH_SIZE * 4];
	std::wcstombs(strCacheFolderPathChar, strCacheFolderPath, sizeof(strCacheFolderPathChar));

	std::stringstream path;
	path << strCacheFolderPathChar << "/" << SHADER_STORAGE_FOLDER_NAME;

	// Convert back to wide chars before using osal
	wchar_t strShaderFolderPath[PLUGIN_PATH_SIZE];
	std::mbstowcs(strShaderFolderPath, path.str().c_str(), PLUGIN_PATH_SIZE);

	if (!osal_path_existsW(strShaderFolderPath) || !osal_is_directory(strShaderFolderPath)) {
		if (osal_mkdirp(strShaderFolderPath) != 0) {
			path.str("");
			path << strCacheFolderPathChar;
		}
	}

	std::string strOpenGLType;

	if(_glinfo.isGLESX) {
		strOpenGLType = "GLES";
	} else {
		strOpenGLType = "OpenGL";
	}

	path << "/GLideN64." << std::hex << static_cast<u32>(std::hash<std::string>()(RSP.romname))
		<< "." << strOpenGLType << "." << _fileExtension;

	return path.str();
}

/*
Storage has text format:
line_1 Version in hex form
line_2 Hardware per pixel lighting support flag
line_3 Count - numbers of combiners keys in hex form
line_4..line_Count+3  combiners keys in hex form, one key per line
*/
bool ShaderStorage::_saveCombinerKeys(const graphics::Combiners & _combiners) const
{
	std::string keysFileName = getStorageFileName(m_glinfo, "keys");

#if defined(OS_WINDOWS) && !defined(MINGW)
	std::ofstream keysOut(keysFileName, std::ofstream::trunc);
#else
	std::ofstream keysOut(keysFileName.c_str(), std::ofstream::trunc);
#endif
	if (!keysOut)
		return false;

	const size_t szCombiners = _combiners.size();

	std::vector<char> allShaderData;
	std::vector<u64> keysData;
	keysData.reserve(szCombiners);
	for (auto cur = _combiners.begin(); cur != _combiners.end(); ++cur)
		keysData.push_back(cur->first.getMux());

	std::sort(keysData.begin(), keysData.end());
	keysOut << "0x" << std::hex << std::setfill('0') << std::setw(8) << m_keysFormatVersion << "\n";
	keysOut << "0x" << std::hex << std::setfill('0') << std::setw(8) << keysData.size() << "\n";
	for (u64 key : keysData)
		keysOut << "0x" << std::hex << std::setfill('0') << std::setw(16) << key << "\n";

	keysOut.flush();
	keysOut.close();
	return true;
}

/*
Storage format:
uint32 - format version;
uint32 - bitset of config options, which may change how shader is created.
uint32 - len of renderer string
char * - renderer string
uint32 - len of GL version string
char * - GL version string
uint32 - number of shaders
shaders in binary form
*/
bool ShaderStorage::saveShadersStorage(const graphics::Combiners & _combiners) const
{
	if (!_saveCombinerKeys(_combiners))
		return false;

	if (gfxContext.isCombinerProgramBuilderObsolete())
		// Created shaders are obsolete due to changes in config, but we saved combiners keys.
		return true;

	if (!graphics::Context::ShaderProgramBinary)
		// Shaders storage is not supported, but we saved combiners keys.
		return true;

	std::string shadersFileName = getStorageFileName(m_glinfo, "shaders");

#if defined(OS_WINDOWS) && !defined(MINGW)
	std::ofstream shadersOut(shadersFileName, std::ofstream::binary | std::ofstream::trunc);
#else
	std::ofstream shadersOut(shadersFileName.c_str(), std::ofstream::binary | std::ofstream::trunc);
#endif
	if (!shadersOut)
		return false;

	displayLoadProgress(L"SAVE COMBINER SHADERS %.1f%%", 0.0f);

	shadersOut.write((char*)&m_formatVersion, sizeof(m_formatVersion));

	const u32 configOptionsBitSet = graphics::CombinerProgram::getShaderCombinerOptionsBits();
	shadersOut.write((char*)&configOptionsBitSet, sizeof(configOptionsBitSet));

	const char * strRenderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
	u32 len = static_cast<u32>(strlen(strRenderer));
	shadersOut.write((char*)&len, sizeof(len));
	shadersOut.write(strRenderer, len);

	const char * strGLVersion = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	len = static_cast<u32>(strlen(strGLVersion));
	shadersOut.write((char*)&len, sizeof(len));
	shadersOut.write(strGLVersion, len);

	const size_t szCombiners = _combiners.size();

	u32 totalWritten = 0;
	std::vector<char> allShaderData;
	std::vector<u64> keysData;

	const f32 percent = szCombiners / 100.0f;
	const f32 step = 100.0f / szCombiners;
	f32 progress = 0.0f;
	f32 percents = percent;

	for (auto cur = _combiners.begin(); cur != _combiners.end(); ++cur)
	{
		std::vector<char> data;
		if (cur->second->getBinaryForm(data))
		{
			allShaderData.insert(allShaderData.end(), data.begin(), data.end());
			++totalWritten;
			progress += step;
			if (progress > percents) {
				displayLoadProgress(L"SAVE COMBINER SHADERS %.1f%%", f32(totalWritten) * 100.f / f32(szCombiners));
				percents += percent;
			}
		}
		else
		{
			LOG(LOG_ERROR, "Error while writing shader with key key=0x%016lX",
				static_cast<long unsigned int>(cur->second->getKey().getMux()));
		}
	}

	shadersOut.write((char*)&totalWritten, sizeof(totalWritten));
	shadersOut.write(allShaderData.data(), allShaderData.size());

	shadersOut.flush();
	shadersOut.close();
	displayLoadProgress(L"");
	return true;
}

static
CombinerProgramImpl * _readCombinerProgramFromStream(std::istream & _is,
	CombinerKey& _cmbKey,
	std::unique_ptr<CombinerProgramUniformFactory> & _uniformFactory,
	opengl::CachedUseProgram * _useProgram)
{
	int inputs;
	_is.read((char*)&inputs, sizeof(inputs));
	CombinerInputs cmbInputs(inputs);

	GLenum binaryFormat;
	GLint  binaryLength;
	_is.read((char*)&binaryFormat, sizeof(binaryFormat));
	_is.read((char*)&binaryLength, sizeof(binaryLength));
	std::vector<char> binary(binaryLength);
	_is.read(binary.data(), binaryLength);

	GLuint program = glCreateProgram();
	const bool isRect = _cmbKey.isRectKey();
	glsl::Utils::locateAttributes(program, isRect, cmbInputs.usesTexture());
	glProgramBinary(program, binaryFormat, binary.data(), binaryLength);
	if (!glsl::Utils::checkProgramLinkStatus(program, true)) {
		return nullptr;
	}

	UniformGroups uniforms;
	_uniformFactory->buildUniforms(program, cmbInputs, _cmbKey, uniforms);

	return new CombinerProgramImpl(_cmbKey, program, _useProgram, cmbInputs, std::move(uniforms));
}

bool ShaderStorage::_loadFromCombinerKeys(graphics::Combiners & _combiners)
{
	std::string keysFileName = getStorageFileName(m_glinfo, "keys");
#if defined(OS_WINDOWS) && !defined(MINGW)
	std::ifstream fin(keysFileName);
#else
	std::ifstream fin(keysFileName.c_str());
#endif
	if (!fin)
		return false;

	u32 version;
	fin >> std::hex >> version;
	if (version < 4)
		return false;

	const bool useGlobalHWLSuport = version == 4;

	if (useGlobalHWLSuport) {
		u32 hwlSupport = 0;
		fin >> std::hex >> hwlSupport;
		GBI.setHWLSupported(hwlSupport != 0);
	}

	displayLoadProgress(L"LOAD COMBINER SHADERS %.1f%%", 0.0f);

	u32 szCombiners;
	fin >> std::hex >> szCombiners;
	const f32 percent = szCombiners / 100.0f;
	const f32 step = 100.0f / szCombiners;
	f32 progress = 0.0f;
	f32 percents = percent;
	u64 mux;
	for (u32 i = 0; i < szCombiners; ++i) {
		fin >> std::hex >> mux;
		CombinerKey key(mux, false);
		if (!useGlobalHWLSuport)
			GBI.setHWLSupported(key.isHWLSupported());
		graphics::CombinerProgram * pCombiner = Combiner_Compile(key.getMux());
		pCombiner->update(true);
		_combiners[pCombiner->getKey()] = pCombiner;
		progress += step;
		if (progress > percents) {
			displayLoadProgress(L"LOAD COMBINER SHADERS %.1f%%", f32(i + 1) * 100.f / f32(szCombiners));
			percents += percent;
		}
	}
	fin.close();

	if (opengl::Utils::isGLError())
		return false;

	if (graphics::Context::ShaderProgramBinary)
		// Restore shaders storage
		return saveShadersStorage(_combiners);

	displayLoadProgress(L"");
	return true;
}

bool ShaderStorage::loadShadersStorage(graphics::Combiners & _combiners)
{
	if (!graphics::Context::ShaderProgramBinary)
		// Shaders storage is not supported, load from combiners keys.
		return _loadFromCombinerKeys(_combiners);

	std::string shadersFileName = getStorageFileName(m_glinfo, "shaders");
	const u32 configOptionsBitSet = graphics::CombinerProgram::getShaderCombinerOptionsBits();

#if defined(OS_WINDOWS) && !defined(MINGW)
	std::ifstream fin(shadersFileName, std::ofstream::binary);
#else
	std::ifstream fin(shadersFileName.c_str(), std::ofstream::binary);
#endif

	if (!fin)
		return _loadFromCombinerKeys(_combiners);

	try {
		u32 version;
		fin.read((char*)&version, sizeof(version));
		if (version != m_formatVersion)
			return _loadFromCombinerKeys(_combiners);

		u32 optionsSet;
		fin.read((char*)&optionsSet, sizeof(optionsSet));
		if (optionsSet != configOptionsBitSet)
			return _loadFromCombinerKeys(_combiners);

		const char * strRenderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
		u32 len;
		fin.read((char*)&len, sizeof(len));
		std::vector<char> strBuf(len);
		fin.read(strBuf.data(), len);
		if (strncmp(strRenderer, strBuf.data(), len) != 0)
			return _loadFromCombinerKeys(_combiners);

		const char * strGLVersion = reinterpret_cast<const char *>(glGetString(GL_VERSION));
		fin.read((char*)&len, sizeof(len));
		strBuf.resize(len);
		fin.read(strBuf.data(), len);
		if (strncmp(strGLVersion, strBuf.data(), len) != 0)
			return _loadFromCombinerKeys(_combiners);

		displayLoadProgress(L"LOAD COMBINER SHADERS %.1f%%", 0.0f);

		std::unique_ptr<CombinerProgramUniformFactory> uniformFactory;

		if (config.generalEmulation.enableInaccurateTextureCoordinates) {
			uniformFactory = std::make_unique<CombinerProgramUniformFactoryFast>(m_glinfo);
		} else {
			uniformFactory = std::make_unique<CombinerProgramUniformFactoryAccurate>(m_glinfo);
		}

		fin.read((char*)&len, sizeof(len));
		const f32 percent = len / 100.0f;
		const f32 step = 100.0f / len;
		f32 progress = 0.0f;
		f32 percents = percent;
		for (u32 i = 0; i < len; ++i) {
			CombinerKey cmbKey;
			cmbKey.read(fin);

			CombinerProgramImpl * pCombiner = _readCombinerProgramFromStream(fin, cmbKey, uniformFactory, m_useProgram);

			if (pCombiner != nullptr) {
				pCombiner->update(true);
				_combiners[pCombiner->getKey()] = pCombiner;
			} else {
				LOG(LOG_ERROR, "Shader is not a valid binary compiling from key instead");
				graphics::CombinerProgram *pCombinerFromKey = Combiner_Compile(cmbKey.getMux());
				pCombinerFromKey->update(true);
				_combiners[cmbKey] = pCombinerFromKey;
			}

			progress += step;
			if (progress > percents) {
				displayLoadProgress(L"LOAD COMBINER SHADERS %.1f%%", f32(i + 1) * 100.f / f32(len) );
				percents += percent;
			}
		}
	} catch (...) {
		LOG(LOG_ERROR, "Stream error while loading shader cache! Buffer is probably not big enough");
	}

	fin.close();
	displayLoadProgress(L"");
	return !opengl::Utils::isGLError();
}


ShaderStorage::ShaderStorage(const opengl::GLInfo & _glinfo, opengl::CachedUseProgram * _useProgram)
: m_glinfo(_glinfo)
, m_useProgram(_useProgram)
{
}
