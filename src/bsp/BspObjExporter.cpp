#include "Bsp.h"
#include "lang.h"
#include "util.h"
#include "log.h"
#include "lodepng.h"
#include "rad.h"
#include "vis.h"
#include "remap.h"
#include "Settings.h"
#include "Renderer.h"
#include "BspRenderer.h"
#include "winding.h"
#include "forcecrc32.h"
#include "quantizer.h"
#include "Wad.h"
#include "Clipper.h"
#include "cli/ProgressMeter.h"
#include "lodepng.h"
#include <string>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>

static bool ends_with_ci(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;

    return std::equal(
        suffix.rbegin(), suffix.rend(),
        str.rbegin(),
        [](char a, char b) {
            return std::tolower(a) == std::tolower(b);
        }
    );
}

static void processMdlToBsp(Bsp* bsp, ProgressMeter& tmp)
{
	int model_count = 0;
	for (size_t ent = 0; ent < bsp->ents.size(); ent++)
	{
		if (bsp->renderer->renderEnts[ent].mdl)
		{
			model_count++;
		}
	}

	tmp.update("MDL TO BSP...", model_count);
	g_progress = tmp;

	for (size_t ent = 0; ent < bsp->ents.size(); ent++)
	{
		if (bsp->renderer->renderEnts[ent].mdl)
		{
			bsp->import_mdl_to_bsp((int)ent, false);

			tmp.tick();
			g_progress = tmp;
		}
	}

	tmp.update("RELOADING MAP...", 8);
	g_progress = tmp;

	tmp.tick();
	g_progress = tmp;
	bsp->remove_unused_model_structures();

	tmp.tick();
	g_progress = tmp;
	bsp->resize_all_lightmaps();

	tmp.tick();
	g_progress = tmp;
	bsp->renderer->reuploadTextures();

	tmp.tick();
	g_progress = tmp;
	bsp->renderer->loadLightmaps();

	tmp.tick();
	g_progress = tmp;
	bsp->renderer->preRenderFaces();

	tmp.tick();
	g_progress = tmp;
	bsp->renderer->pushUndoState("CREATE MDL->BSP MODEL", EDIT_MODEL_LUMPS | FL_ENTITIES);
}

static bool isSemiTransparentTexture(const std::string& texName)
{
	std::string lowerName = toLowerCase(texName);
	return lowerName == "aaatrigger" ||
		   lowerName == "null" ||
		   starts_with(lowerName, "sky") ||
		   lowerName == "noclip" ||
		   lowerName == "clip" ||
		   lowerName == "origin" ||
		   lowerName == "bevel" ||
		   lowerName == "hint" ||
		   lowerName == "skip";
}

static int exportMaterial(const std::string& path, std::vector<std::string>& materials, std::vector<std::string>& matnames,
						  std::string folder, std::string filename, COLOR4* rgba_data, int width, int height)
{
	// build path pieces
	std::string relative_path = folder + "/" + filename + ".png";
	std::string full_path = path + relative_path;
	createDir(path + folder);

	// check if material already exists
	for (size_t i = 0; i < matnames.size(); i++)
	{
		if (matnames[i] == filename)
		{
			return (int)i;
		}
	}

	// convert and export to png
	if (!fileExists(full_path))
	{
		// save to png
		lodepng_encode32_file(full_path.c_str(), (unsigned char*)rgba_data, width, height);

		print_log("Dumped material {} to {}\n", filename, full_path);
	}

	// add to obj materials
	int materialIndex = (int)matnames.size();
	matnames.push_back(filename);
	materials.emplace_back("");
	materials.emplace_back("newmtl " + filename);
	materials.emplace_back("Ns 0");
	materials.emplace_back("Ka 1 1 1");
	materials.emplace_back("Ks 0 0 0");
	materials.emplace_back("Ke 0 0 0");

	materials.emplace_back("Ni 1");

	if (isSemiTransparentTexture(filename))
	{
		materials.emplace_back("d 0.25");
		materials.emplace_back("illum 1");
	}
	else
	{
		materials.emplace_back("d 1");
		materials.emplace_back("illum 2");
	}

	materials.emplace_back("map_Kd " + relative_path);

	return materialIndex;
}

static void exportLightmaps(const std::string& path, std::string& bsp_name, std::vector<std::string>& materials, std::vector<std::string>& matnames, BspRenderer* bsprend)
{
	for (size_t i = 0; i < bsprend->glLightmapTextures.size(); i++)
	{
		Texture* atlas_tex = bsprend->glLightmapTextures[i];
		if (!atlas_tex || !atlas_tex->get_data()) { continue; }

		// get rgb texture
		COLOR3* rgb_data = (COLOR3*)atlas_tex->get_data();

		// convert to rgba
		int num_pixels = atlas_tex->width * atlas_tex->height;
		COLOR4* rgba_data = new COLOR4[num_pixels];
		for (int j = 0; j < num_pixels; j++) {
			rgba_data[j] = COLOR4(rgb_data[j].r, rgb_data[j].g, rgb_data[j].b, 255);
		}

		// export
		exportMaterial(
			path, materials, matnames,
			"atlases", bsp_name + "_atlas_" + std::to_string(i),
			rgba_data, atlas_tex->width, atlas_tex->height
		);

		delete rgba_data;
	}
}

static void exportSkybox(const std::string& path, std::string& bsp_name, std::vector<std::string>& materials, std::vector<std::string>& matnames, Bsp* bsp, BspRenderer* bsprend)
{
	// Get skyname from worldspawn entity
	std::string skyName;
	if (!bsp->ents.empty() && bsp->ents[0]->keyvalues.count("skyname"))
	{
		skyName = bsp->ents[0]->keyvalues["skyname"];
	}

	if (skyName.empty())
	{
		return;
	}

	// For the skyname, copy the 6 faces as TGA files
	std::vector<std::string> suffixes = {"ft", "bk", "lf", "rt", "up", "dn"};

	createDir(path + "skyboxes");

	for (const auto& pathToggle : g_settings.resPaths)
	{
		std::string skyboxPath = pathToggle.path + "/gfx/env/" + skyName;

		for (const std::string& suffix : suffixes)
		{
			std::string sourceFile = skyboxPath + suffix + ".tga";
			std::string destFile = path + "skyboxes/" + skyName + suffix + ".tga";

			if (fileExists(sourceFile) && !fileExists(destFile))
			{
				// Copy the TGA file
				std::ifstream src(sourceFile, std::ios::binary);
				std::ofstream dst(destFile, std::ios::binary);
				if (src && dst)
				{
					dst << src.rdbuf();
					print_log("Copied skybox texture {} to {}\n", skyName + suffix + ".tga", destFile);
				}
			}
		}
	}
}

static int addTextureMaterial(const std::string& path, std::vector<std::string>& materials, std::vector<std::string>& matnames,
							   Bsp* bsp, BSPMIPTEX& tex, BSPTEXTUREINFO& texinfo, int texOffset)
{
	// set default value
	int materialid = -1;

	// check if material already exists
	for (size_t i = 0; i < matnames.size(); i++)
	{
		if (matnames[i] == tex.szName)
		{
			return (int)i;
		}
	}

	// store/export mip texture
	if (tex.nOffsets[0] > 0)
	{
		if (texOffset >= 0)
		{
			int color_count = (g_settings.pal_id >= 0)
				? g_settings.palettes[g_settings.pal_id].colors
				: 256;

			COLOR3* palette = (g_settings.pal_id >= 0)
				? (COLOR3*)&g_settings.palettes[g_settings.pal_id].data
				: (COLOR3*)&g_settings.palette_default;

			COLOR4* rgba_data = ConvertMipTexToRGBA(((BSPMIPTEX*)(bsp->textures + texOffset)), bsp->is_texture_with_pal(texinfo.iMiptex) ? NULL : palette, color_count);
			materialid = exportMaterial(
				path, materials, matnames,
				"textures", tex.szName,
				rgba_data, tex.nWidth, tex.nHeight
			);
			delete rgba_data;
		}
		return materialid;
	}

	// store/export wad texture
	for (size_t r = 0; r < mapRenderers.size(); r++)
	{
		for (size_t k = 0; k < mapRenderers[r]->wads.size(); k++)
		{
			if (mapRenderers[r]->wads[k]->hasTexture(tex.szName))
			{
				WADTEX* wadTex = mapRenderers[r]->wads[k]->readTexture(tex.szName);

				COLOR4* rgba_data = ConvertWadTexToRGBA(wadTex);
				materialid = exportMaterial(
					path, materials, matnames,
					"textures", tex.szName,
					rgba_data, tex.nWidth, tex.nHeight
				);
				delete rgba_data;

				return materialid;
			}
		}
	}

	return materialid;
}

static int addMdlTextureMaterial(const std::string& path, std::vector<std::string>& materials, std::vector<std::string>& matnames,
								 Texture* tex, const std::string& texname)
{
	// check if material already exists
	for (size_t i = 0; i < matnames.size(); i++)
	{
		if (matnames[i] == texname)
		{
			return (int)i;
		}
	}

	// export to png
	int materialid = exportMaterial(
		path, materials, matnames,
		"textures", texname,
		(COLOR4*)tex->get_data(), tex->width, tex->height
	);
	return materialid;
}

static mstudiotexture_t* getMdlTextureInfo(StudioModel* mdl, Texture* tex)
{
    if (!mdl || !tex || !mdl->m_ptexturehdr || mdl->m_ptexturehdr->numtextures <= 0) {
        return nullptr;
    }

    // Get pointer to texture info array
    mstudiotexture_t* mdlTexInfo = (mstudiotexture_t*)((unsigned char*)mdl->m_ptexturehdr + mdl->m_ptexturehdr->textureindex);

    // Iterate through texture info and find the one matching our Texture*
    for (int i = 0; i < mdl->m_ptexturehdr->numtextures; i++) {
        // mstudiotexture_t::index stores the corresponding index in mdl->mdl_textures
        if (mdlTexInfo[i].index >= 0 && mdlTexInfo[i].index < (int)mdl->mdl_textures.size() &&
            mdl->mdl_textures[mdlTexInfo[i].index] == tex) {
            return &mdlTexInfo[i];
        }
    }

    // Fallback: match by texture name
    for (int i = 0; i < mdl->m_ptexturehdr->numtextures; i++) {
        std::string mdlTexName = stripExt(mdlTexInfo[i].name);
        if (mdlTexName == tex->texName) {
            return &mdlTexInfo[i];
        }
    }

    return nullptr;
}

static std::string json_escape(const std::string& s) {
    std::stringstream ss;
    for (char c : s) {
        switch (c) {
            case '"' : ss << "\\\""; break;
            case '\\' : ss << "\\\\"; break;
            case '\b' : ss << "\\b"; break;
            case '\f' : ss << "\\f"; break;
            case '\n' : ss << "\\n"; break;
            case '\r' : ss << "\\r"; break;
            case '\t' : ss << "\\t"; break;
            default: if (c < 32) ss << "\\u" << std::hex << std::setfill('0') << std::setw(4) << (int)c << std::dec; else ss << c;
        }
    }
    return ss.str();
}

static void exportMdlJson(const std::string& base_path, StudioModel* mdl, const std::string& model_name)
{
	if (!mdl || !mdl->m_pstudiohdr) return;

	std::string json_path = base_path + "/mdl.json";

	std::stringstream json;
	json << "{\n";
	json << "    \"model_name\":\"" << model_name << "\",\n";
	json << "    \"header\": {\n";
	json << "        \"id\":\"" << std::hex << mdl->m_pstudiohdr->id << "\",\n";
	json << "        \"version\":" << std::dec << mdl->m_pstudiohdr->version << ",\n";
	json << "        \"name\":\"" << json_escape(mdl->m_pstudiohdr->name) << "\",\n";
	json << "        \"length\":" << mdl->m_pstudiohdr->length << ",\n";
	json << "        \"eyeposition\":[" << mdl->m_pstudiohdr->eyeposition.x << "," << mdl->m_pstudiohdr->eyeposition.y << "," << mdl->m_pstudiohdr->eyeposition.z << "],\n";
	json << "        \"min\":[" << mdl->m_pstudiohdr->min.x << "," << mdl->m_pstudiohdr->min.y << "," << mdl->m_pstudiohdr->min.z << "],\n";
	json << "        \"max\":[" << mdl->m_pstudiohdr->max.x << "," << mdl->m_pstudiohdr->max.y << "," << mdl->m_pstudiohdr->max.z << "],\n";
	json << "        \"bbmin\":[" << mdl->m_pstudiohdr->bbmin.x << "," << mdl->m_pstudiohdr->bbmin.y << "," << mdl->m_pstudiohdr->bbmin.z << "],\n";
	json << "        \"bbmax\":[" << mdl->m_pstudiohdr->bbmax.x << "," << mdl->m_pstudiohdr->bbmax.y << "," << mdl->m_pstudiohdr->bbmax.z << "],\n";
	json << "        \"flags\":" << mdl->m_pstudiohdr->flags << ",\n";
	json << "        \"numbones\":" << mdl->m_pstudiohdr->numbones << ",\n";
	json << "        \"numbonecontrollers\":" << mdl->m_pstudiohdr->numbonecontrollers << ",\n";
	json << "        \"numhitboxes\":" << mdl->m_pstudiohdr->numhitboxes << ",\n";
	json << "        \"numseq\":" << mdl->m_pstudiohdr->numseq << ",\n";
	json << "        \"numseqgroups\":" << mdl->m_pstudiohdr->numseqgroups << ",\n";
	json << "        \"numtextures\":" << mdl->m_pstudiohdr->numtextures << ",\n";
	json << "        \"numskinref\":" << mdl->m_pstudiohdr->numskinref << ",\n";
	json << "        \"numskinfamilies\":" << mdl->m_pstudiohdr->numskinfamilies << ",\n";
	json << "        \"numbodyparts\":" << mdl->m_pstudiohdr->numbodyparts << ",\n";
	json << "        \"numattachments\":" << mdl->m_pstudiohdr->numattachments << "\n";
	json << "    },\n";
	json << "    \"textures\": {\n";

	if (mdl->m_ptexturehdr && mdl->m_ptexturehdr->numtextures > 0) {
		mstudiotexture_t* mdlTexInfo = (mstudiotexture_t*)((unsigned char*)mdl->m_pstudiohdr + mdl->m_ptexturehdr->textureindex);

		for (int i = 0; i < mdl->m_ptexturehdr->numtextures; i++) {
			if (i > 0) json << ",\n";
			std::string texName = stripExt(mdlTexInfo[i].name);
			json << "        \"" << texName << "\": {\n";
			json << "            \"flags\":" << mdlTexInfo[i].flags << ",\n";
			json << "            \"width\":" << mdlTexInfo[i].width << ",\n";
			json << "            \"height\":" << mdlTexInfo[i].height << "\n";
			json << "        }";
		}
	}

	json << "\n    },\n";
	json << "    \"bodyparts\": [\n";

	mstudiobodyparts_t* pbodypart = (mstudiobodyparts_t*)((unsigned char*)mdl->m_pstudiohdr + mdl->m_pstudiohdr->bodypartindex);
	for (int bg = 0; bg < mdl->m_pstudiohdr->numbodyparts; bg++)
	{
		if (bg > 0) json << ",\n";
		json << "        {\n";
		json << "            \"name\":\"" << pbodypart->name << "\",\n";
		json << "            \"nummodels\":" << pbodypart->nummodels << ",\n";
		json << "            \"base\":" << pbodypart->base << "\n";
		json << "        }";
		pbodypart++;
	}

	json << "\n    ],\n";
	json << "    \"total_body_configs\":" << (mdl->GetBodyCount() + 1) << ",\n";
	json << "    \"sequences\": [\n";

	mstudioseqdesc_t* pseqdesc = (mstudioseqdesc_t*)((unsigned char*)mdl->m_pstudiohdr + mdl->m_pstudiohdr->seqindex);
	for (int sq = 0; sq < mdl->m_pstudiohdr->numseq; sq++)
	{
		if (sq > 0) json << ",\n";
		json << "        {\n";
		json << "            \"label\":\"" << pseqdesc->label << "\",\n";
		json << "            \"fps\":" << pseqdesc->fps << ",\n";
		json << "            \"flags\":" << pseqdesc->flags << ",\n";
		json << "            \"activity\":" << pseqdesc->activity << ",\n";
		json << "            \"actweight\":" << pseqdesc->actweight << ",\n";
		json << "            \"numevents\":" << pseqdesc->numevents << ",\n";
		json << "            \"numframes\":" << pseqdesc->numframes << ",\n";
		json << "            \"numpivots\":" << pseqdesc->numpivots << ",\n";
		json << "            \"motiontype\":" << pseqdesc->motiontype << ",\n";
		json << "            \"bonemotion\":" << pseqdesc->motionbone << ",\n";
		json << "            \"linearmovement\":[" << pseqdesc->linearmovement.x << "," << pseqdesc->linearmovement.y << "," << pseqdesc->linearmovement.z << "],\n";
		json << "            \"bbmin\":[" << pseqdesc->bbmin.x << "," << pseqdesc->bbmin.y << "," << pseqdesc->bbmin.z << "],\n";
		json << "            \"bbmax\":[" << pseqdesc->bbmax.x << "," << pseqdesc->bbmax.y << "," << pseqdesc->bbmax.z << "],\n";
		json << "            \"numblends\":" << pseqdesc->numblends << ",\n";
		json << "            \"seqgroup\":" << pseqdesc->seqgroup << ",\n";
		json << "            \"entrynode\":" << pseqdesc->entrynode << ",\n";
		json << "            \"exitnode\":" << pseqdesc->exitnode << "\n";
		json << "        }";
		pseqdesc++;
	}

	json << "\n    ]\n";
	json << "}\n";

	createDir(base_path);
	std::ofstream mdl_file(base_path + "/mdl.json");
	if (mdl_file.is_open())
	{
		mdl_file << json.str();
		mdl_file.close();
	}
}

static void exportMdlToObj(const std::string& base_path, StudioModel* mdl, const std::string& obj_name, const std::string& mtl_name, float scale, ProgressMeter& tmp)
{
	createDir(base_path);

	std::string obj_file_path = base_path + obj_name + ".obj";
	std::string mtl_file_path = base_path + mtl_name + ".mtl";

	std::vector<std::string> materials;
	std::vector<std::string> matnames;

	int vertoffset = 1;
	int texoffset = 1;

	std::ofstream obj_file(obj_file_path);
	if (!obj_file) return;

	obj_file << "# Exported MDL using bspguy!\n";
	obj_file << "mtllib " << mtl_name << ".mtl\n";

	std::ofstream mtl_file(mtl_file_path);
	if (mtl_file) {
		mtl_file << "# Exported MDL mtl using bspguy!\n";
	}

	std::map<Texture*, int> texToMatId;
	int lastMaterial = -1;

	for (size_t group = 0; group < mdl->mdl_mesh_groups.size(); group++) {
		for (size_t meshid = 0; meshid < mdl->mdl_mesh_groups[group].size(); meshid++) {
			StudioMesh& sm = mdl->mdl_mesh_groups[group][meshid];

			// Ensure material for this texture
			int currentMaterial = -1;
			if (sm.texture) {
				std::string texname = sm.texture->texName.empty() ? "unnamed" : sm.texture->texName;
				if (texToMatId.find(sm.texture) == texToMatId.end()) {
					currentMaterial = addMdlTextureMaterial(base_path, materials, matnames, sm.texture, texname);
					texToMatId[sm.texture] = currentMaterial;
				} else {
					currentMaterial = texToMatId[sm.texture];
				}
			} else {
				// No texture, use null or something, but for simplicity, skip or use default
				continue;
			}

			// Switch material if changed
			if (currentMaterial != lastMaterial) {
				if (currentMaterial >= 0 && currentMaterial < (int)matnames.size()) {
					obj_file << "usemtl " << matnames[currentMaterial] << "\n";
				}
				lastMaterial = currentMaterial;
			}

			for (auto& v : sm.verts) {
				vec3 pos = v.pos * scale;
				obj_file << "v " << pos.toKeyvalueString() << "\n";
			}

			for (auto& v : sm.verts) {
				obj_file << "vt " << v.u << " " << (1.0f - v.v) << "\n";
			}

			// Assume triangles
			for (size_t i = 0; i < sm.verts.size(); i += 3) {
				obj_file << "f";
				for (int j = 2; j >= 0; j--) {
					size_t idx = i + j;
					if (idx >= sm.verts.size()) break;
					obj_file << " " << vertoffset + idx << "/" << texoffset + idx;
				}
				obj_file << "\n";
			}

			vertoffset += sm.verts.size();
			texoffset += sm.verts.size();
		}
	}

	if (mtl_file) {
		for (auto const& s : materials) {
			mtl_file << s << '\n';
		}
		mtl_file.close();
	}

	obj_file.close();
}

static void exportSeparateMdls(const std::string& path, float scale, Bsp* bsp, ProgressMeter& tmp)
{
	int mdl_count = 0;
	int total_bodies = 0;
	for (size_t ent = 0; ent < bsp->ents.size(); ent++)
	{
		if (bsp->renderer->renderEnts[ent].mdl)
		{
			mdl_count++;
			StudioModel* mdl = (StudioModel*)bsp->renderer->renderEnts[ent].mdl;
			total_bodies += mdl->GetBodyCount() + 1;
		}
	}
	if (mdl_count > 0) {
		tmp.update("EXPORT MDL...", total_bodies);
		g_progress = tmp;
		for (size_t ent = 0; ent < bsp->ents.size(); ent++)
		{
			if (bsp->renderer->renderEnts[ent].mdl)
			{
				StudioModel* mdl = (StudioModel*)bsp->renderer->renderEnts[ent].mdl;
				std::string model_path = bsp->ents[ent]->keyvalues["model"];
				if (!ends_with_ci(model_path, ".mdl")) { continue; }

				// Flatten path for folder name
				std::string adjusted_path = model_path;
				replaceAll(adjusted_path, "/", "_");
				replaceAll(adjusted_path, ".", "_");

				std::string model_folder = path + "/mdl_models/" + adjusted_path + "/";
				createDir(model_folder);

				exportMdlJson(model_folder, mdl, adjusted_path);

				int bodyCount = mdl->GetBodyCount() + 1;
				for(int body=0; body < bodyCount; body++){
					std::ostringstream body_num;
					body_num << std::setfill('0') << std::setw(3) << body;
					std::string body_str = "body" + body_num.str();

					mdl->SetBody(body);
					mdl->mdl_mesh_groups = std::vector<std::vector<StudioMesh>>();
					mdl->UpdateModelMeshList();

					exportMdlToObj(model_folder, mdl, body_str, body_str, scale, tmp);
					tmp.tick();
				}
			}
		}
	} else {
		bsp->renderer->preRenderEnts();
	}
}

static void exportSpr(const std::string& output_path, Sprite* spr, const std::string& name, ProgressMeter& tmp)
{
	std::string path = output_path + "/sprites/" + name + "/";
	createDir(path);

	bool intensity_alpha = spr->header.texFormat == 1;

	// Export "sprite.json" with header and all frameinfo data
	std::stringstream json;
	json << "{\n";
	json << "    \"name\":\"" << spr->name << "\",\n";
	json << "    \"header\": {\n";
	json << "        \"ident\":" << spr->header.ident << ",\n";
	json << "        \"version\":" << spr->header.version << ",\n";
	json << "        \"type\":" << spr->header.type << ",\n";
	json << "        \"texFormat\":" << spr->header.texFormat << ",\n";
	json << "        \"boundingradius\":" << spr->header.boundingradius << ",\n";
	json << "        \"width\":" << spr->header.width << ",\n";
	json << "        \"height\":" << spr->header.height << ",\n";
	json << "        \"numframes\":" << spr->header.numframes << ",\n";
	json << "        \"beamlength\":" << spr->header.beamlength << ",\n";
	json << "        \"synctype\":" << static_cast<int>(spr->header.synctype) << "\n";
	json << "    },\n";
	json << "    \"groups\": [\n";

	for (size_t g = 0; g < spr->sprite_groups.size(); g++) {
		if (g > 0) json << ",\n";
		json << "        {\n";
		json << "            \"frames\": [\n";

		for (size_t f = 0; f < spr->sprite_groups[g].sprites.size(); f++) {
			SpriteImage& img = spr->sprite_groups[g].sprites[f];
			if (f > 0) json << ",\n";
			json << "                {\n";
			json << "                    \"origin\":[" << img.frameinfo.origin[0] << "," << img.frameinfo.origin[1] << "],\n";
			json << "                    \"width\":" << img.frameinfo.width << ",\n";
			json << "                    \"height\":" << img.frameinfo.height << "\n";
			json << "                }";
		}

		json << "\n            ]\n";
		json << "        }";
	}

	json << "\n    ]\n";
	json << "}\n";

	std::ofstream sprite_file(path + "sprite.json");
	if (sprite_file.is_open()) {
		sprite_file << json.str();
		sprite_file.close();
	}

	// Export each png
	for (size_t g = 0; g < spr->sprite_groups.size(); g++) {
		for (size_t f = 0; f < spr->sprite_groups[g].sprites.size(); f++) {
			tmp.tick();

			SpriteImage& img = spr->sprite_groups[g].sprites[f];

			std::string g_str = (std::ostringstream() << std::setfill('0') << std::setw(3) << g).str();
			std::string f_str = (std::ostringstream() << std::setfill('0') << std::setw(3) << f).str();
			std::string full_path = path + g_str + "_" + f_str + ".png";

			if (!fileExists(full_path))
			{
				// convert to unsigned char* for lodepng
				int num_pixels = img.frameinfo.width * img.frameinfo.height;
				std::vector<unsigned char> rgba_bytes(num_pixels * 4);
				for (int p = 0; p < num_pixels; p++) {
					rgba_bytes[p * 4 + 0] = img.image[p].r;
					rgba_bytes[p * 4 + 1] = img.image[p].g;
					rgba_bytes[p * 4 + 2] = img.image[p].b;
					if (intensity_alpha) {
						float alpha = (img.image[p].r + img.image[p].g + img.image[p].b) / (3.0f * 255.0f);
						alpha = 1.0f - powf(1.0f - alpha, 3);
						rgba_bytes[p * 4 + 3] = (int)(alpha * 255);
					} else {
						rgba_bytes[p * 4 + 3] = img.image[p].a;
					}
				}

				lodepng_encode32_file(full_path.c_str(), rgba_bytes.data(), img.frameinfo.width, img.frameinfo.height);
			}
		}
	}
}

static void exportSeparateSprs(const std::string& path, float scale, Bsp* bsp, ProgressMeter& tmp)
{
	int spr_count = 0;
	for (size_t ent = 0; ent < bsp->ents.size(); ent++)
	{
		if (bsp->renderer->renderEnts[ent].spr)
		{
			spr_count++;
		}
	}
	if (spr_count > 0) {
		tmp.update("EXPORT SPR...", spr_count);
		g_progress = tmp;
		for (size_t ent = 0; ent < bsp->ents.size(); ent++)
		{
			if (bsp->renderer->renderEnts[ent].spr)
			{
				Sprite* spr = (Sprite*)bsp->renderer->renderEnts[ent].spr;
				std::string model_path = bsp->ents[ent]->keyvalues["model"];
				if (!ends_with_ci(model_path, ".spr")) { continue; }

				// Replace slashes with underscores to flatten path
				std::string adjusted_path = model_path;
				replaceAll(adjusted_path, "/", "_");
				replaceAll(adjusted_path, ".", "_");

				std::string model_name = adjusted_path.empty() ? "unknown" : adjusted_path;
				exportSpr(path, spr, model_name, tmp);
			}
		}
	}
}

static void exportCollisionGeometry(Bsp* bsp,
	std::vector<std::string>& group_list,
	std::map<std::string, std::stringstream>& group_verts,
	std::map<std::string, std::stringstream>& group_normals,
	std::map<std::string, std::stringstream>& group_textures,
	std::map<std::string, std::stringstream>& group_objects,
	std::map<std::string, int>& group_vert_groups,
	std::vector<std::string>& matnames,
	int& vertoffset,
	int& normoffset,
	float scale,
	int grouping,
	int& materialid,
	int& lastmaterialid,
	ProgressMeter& tmp,
	int modelCount,
	int null_material_idx)
{
	int hullIdx = 1;
	std::string collisionGroupName = "collision_hull_" + std::to_string(hullIdx);

	if (std::find(group_list.begin(), group_list.end(), collisionGroupName) == group_list.end())
		group_list.push_back(collisionGroupName);

	materialid = null_material_idx;
	if (lastmaterialid != null_material_idx) {
		group_vert_groups[collisionGroupName]++;
		if (grouping == 1) {
			group_objects[collisionGroupName] << "g " << collisionGroupName << "_f" << group_vert_groups[collisionGroupName] << "\n";
		}
		else if (grouping == 2) {
			group_objects[collisionGroupName] << "o " << collisionGroupName << "_f" << group_vert_groups[collisionGroupName] << "\n";
		}
		if (materialid >= 0 && materialid < (int)matnames.size()) {
			group_objects[collisionGroupName] << "usemtl " << matnames[materialid] << "\n";
		}
	}

	tmp.update("Export collision geometry...", modelCount);
	g_progress = tmp;

	for (int modelIdx = 0; modelIdx < modelCount; modelIdx++) {
		tmp.tick();
		g_progress = tmp;

		Clipper clipper;
		std::vector<NodeVolumeCuts> solidNodes = bsp->get_model_leaf_volume_cuts(modelIdx, hullIdx, CONTENTS_SOLID);

		for (size_t k = 0; k < solidNodes.size(); k++) {
			CMesh mesh = clipper.clip(solidNodes[k].cuts);

			if (mesh.verts.empty() || mesh.faces.empty()) {
				continue;
			}

			// Get entity offset for this model
			std::vector<int> entIds = bsp->get_model_ents_ids(modelIdx);
			vec3 origin_offset = vec3();

			if (!entIds.empty() && entIds[0] < (int)bsp->ents.size()) {
				Entity* ent = bsp->ents[entIds[0]];
				origin_offset = ent->origin;
			}

			// Build vertex index mapping for visible verts
			std::map<int, int> vertIndexMap;
			int localVertCount = 0;

			for (size_t v = 0; v < mesh.verts.size(); v++) {
				if (mesh.verts[v].visible) {
					vertIndexMap[v] = vertoffset + localVertCount;
					localVertCount++;

					vec3 pos = (mesh.verts[v].pos + origin_offset) * scale;
					pos = pos.flip();
					group_verts[collisionGroupName] << "v " << pos.toKeyvalueString() << "\n";
				}
			}

			// Process each visible face
			for (size_t f = 0; f < mesh.faces.size(); f++) {
				CFace& face = mesh.faces[f];
				if (!face.visible || face.edges.empty()) continue;

				// Build ordered vertex loop by walking edges
				std::vector<int> faceVerts;
				std::set<int> visitedEdges;

				// Start with first edge
				int currentEdge = face.edges[0];
				CEdge& startEdge = mesh.edges[currentEdge];
				faceVerts.push_back(startEdge.verts[0]);
				faceVerts.push_back(startEdge.verts[1]);
				visitedEdges.insert(currentEdge);

				// Walk remaining edges to build ordered loop
				while (visitedEdges.size() < face.edges.size()) {
					int lastVert = faceVerts.back();
					bool foundNext = false;

					for (size_t e = 0; e < face.edges.size(); e++) {
						int edgeIdx = face.edges[e];
						if (visitedEdges.count(edgeIdx)) continue;

						CEdge& edge = mesh.edges[edgeIdx];
						if (edge.verts[0] == lastVert) {
							faceVerts.push_back(edge.verts[1]);
							visitedEdges.insert(edgeIdx);
							foundNext = true;
							break;
						}
						else if (edge.verts[1] == lastVert) {
							faceVerts.push_back(edge.verts[0]);
							visitedEdges.insert(edgeIdx);
							foundNext = true;
							break;
						}
					}

					if (!foundNext) break;
				}

				// Remove duplicate last vertex if loop closed
				if (faceVerts.size() > 2 && faceVerts.front() == faceVerts.back()) {
					faceVerts.pop_back();
				}

				// Need at least 3 vertices for a valid face
				if (faceVerts.size() < 3) continue;

				// Write normal for this face
				vec3 normal = face.normal.flip();
				group_normals[collisionGroupName] << "vn " << normal.toKeyvalueString() << "\n";
				normoffset++;

				// Write dummy UVs for each vertex
				for (size_t v = 0; v < faceVerts.size(); v++) {
					group_textures[collisionGroupName] << "vt 0.0 0.0\n";
				}

				// Triangulate the face (simple fan triangulation)
				for (size_t v = 1; v < faceVerts.size() - 1; v++) {
					group_objects[collisionGroupName] << "f";

					// Triangle: 0, v, v+1
					for (int idx : {0, (int)v, (int)v + 1}) {
						int globalVertIdx = vertIndexMap[faceVerts[idx]];
						group_objects[collisionGroupName] << " " << globalVertIdx << "/" << globalVertIdx << "/" << normoffset;
					}

					group_objects[collisionGroupName] << "\n";
				}
			}

			vertoffset += localVertCount;
		}
	}

	lastmaterialid = materialid;
}

static void exportModelClipnodeGeometry(Bsp* bsp,
	std::vector<std::string>& group_list,
	std::map<std::string, std::stringstream>& group_verts,
	std::map<std::string, std::stringstream>& group_normals,
	std::map<std::string, std::stringstream>& group_textures,
	std::map<std::string, std::stringstream>& group_objects,
	std::map<std::string, int>& group_vert_groups,
	std::vector<std::string>& matnames,
	int& vertoffset,
	int& normoffset,
	float scale,
	int grouping,
	int& materialid,
	int& lastmaterialid,
	ProgressMeter& tmp,
	int modelIdx,
	int hullIdx,
	int null_material_idx)
{
	// Get entity info for group naming
	std::vector<int> entIds = bsp->get_model_ents_ids(modelIdx);
	int tmpentid = 0; // worldspawn if no entity
	std::string classname = "worldspawn";
	if (!entIds.empty() && entIds[0] < (int)bsp->ents.size()) {
		tmpentid = entIds[0];
		classname = bsp->ents[tmpentid]->classname;
	}
	std::string clipnodeGroupName = "M_" + std::to_string(modelIdx) + "_ENT_" + std::to_string(tmpentid) + "#clipnode#" + classname;

	if (std::find(group_list.begin(), group_list.end(), clipnodeGroupName) == group_list.end())
		group_list.push_back(clipnodeGroupName);

	materialid = null_material_idx;
	if (lastmaterialid != null_material_idx) {
		group_vert_groups[clipnodeGroupName]++;
		if (grouping == 1) {
			group_objects[clipnodeGroupName] << "g " << clipnodeGroupName << "_f" << group_vert_groups[clipnodeGroupName] << "\n";
		}
		else if (grouping == 2) {
			group_objects[clipnodeGroupName] << "o " << clipnodeGroupName << "_f" << group_vert_groups[clipnodeGroupName] << "\n";
		}
		if (materialid >= 0 && materialid < (int)matnames.size()) {
			group_objects[clipnodeGroupName] << "usemtl " << matnames[materialid] << "\n";
		}
	}

	tmp.update("Export clipnode for model " + std::to_string(modelIdx) + "...", 1);
	g_progress = tmp;

	Clipper clipper;
	std::vector<NodeVolumeCuts> solidNodes = bsp->get_model_leaf_volume_cuts(modelIdx, hullIdx, CONTENTS_SOLID);

	for (size_t k = 0; k < solidNodes.size(); k++) {
		tmp.tick();
		g_progress = tmp;

		CMesh mesh = clipper.clip(solidNodes[k].cuts);

		if (mesh.verts.empty() || mesh.faces.empty()) {
			continue;
		}

		// Get entity offset for this model
		std::vector<int> entIds = bsp->get_model_ents_ids(modelIdx);
		vec3 origin_offset = vec3();

		if (!entIds.empty() && entIds[0] < (int)bsp->ents.size()) {
			Entity* ent = bsp->ents[entIds[0]];
			origin_offset = ent->origin;
		}

		// Build vertex index mapping for visible verts
		std::map<int, int> vertIndexMap;
		int localVertCount = 0;

		for (size_t v = 0; v < mesh.verts.size(); v++) {
			if (mesh.verts[v].visible) {
				vertIndexMap[v] = vertoffset + localVertCount;
				localVertCount++;

				vec3 pos = (mesh.verts[v].pos + origin_offset) * scale;
				pos = pos.flip();
				group_verts[clipnodeGroupName] << "v " << pos.toKeyvalueString() << "\n";
			}
		}

		// Process each visible face
		for (size_t f = 0; f < mesh.faces.size(); f++) {
			CFace& face = mesh.faces[f];
			if (!face.visible || face.edges.empty()) continue;

			// Build ordered vertex loop by walking edges
			std::vector<int> faceVerts;
			std::set<int> visitedEdges;

			// Start with first edge
			int currentEdge = face.edges[0];
			CEdge& startEdge = mesh.edges[currentEdge];
			faceVerts.push_back(startEdge.verts[0]);
			faceVerts.push_back(startEdge.verts[1]);
			visitedEdges.insert(currentEdge);

			// Walk remaining edges to build ordered loop
			while (visitedEdges.size() < face.edges.size()) {
				int lastVert = faceVerts.back();
				bool foundNext = false;

				for (size_t e = 0; e < face.edges.size(); e++) {
					int edgeIdx = face.edges[e];
					if (visitedEdges.count(edgeIdx)) continue;

					CEdge& edge = mesh.edges[edgeIdx];
					if (edge.verts[0] == lastVert) {
						faceVerts.push_back(edge.verts[1]);
						visitedEdges.insert(edgeIdx);
						foundNext = true;
						break;
					}
					else if (edge.verts[1] == lastVert) {
						faceVerts.push_back(edge.verts[0]);
						visitedEdges.insert(edgeIdx);
						foundNext = true;
						break;
					}
				}

				if (!foundNext) break;
			}

			// Remove duplicate last vertex if loop closed
			if (faceVerts.size() > 2 && faceVerts.front() == faceVerts.back()) {
				faceVerts.pop_back();
			}

			// Need at least 3 vertices for a valid face
			if (faceVerts.size() < 3) continue;

			// Write normal for this face
			vec3 normal = face.normal.flip();
			group_normals[clipnodeGroupName] << "vn " << normal.toKeyvalueString() << "\n";
			normoffset++;

			// Write dummy UVs for each vertex
			for (size_t v = 0; v < faceVerts.size(); v++) {
				group_textures[clipnodeGroupName] << "vt 0.0 0.0\n";
			}

			// Triangulate the face (simple fan triangulation)
			for (size_t v = 1; v < faceVerts.size() - 1; v++) {
				group_objects[clipnodeGroupName] << "f";

				// Triangle: 0, v, v+1
				for (int idx : {0, (int)v, (int)v + 1}) {
					int globalVertIdx = vertIndexMap[faceVerts[idx]];
					group_objects[clipnodeGroupName] << " " << globalVertIdx << "/" << globalVertIdx << "/" << normoffset;
				}

				group_objects[clipnodeGroupName] << "\n";
			}
		}

		vertoffset += localVertCount;
	}

	lastmaterialid = materialid;
}

void Bsp::ExportToObjWIP(const std::string& path, int iscale, bool lightmapmode, bool with_mdl, bool export_csm, int grouping, bool export_collision)
{
	if (!createDir(path))
	{
		print_log(PRINT_RED | PRINT_INTENSITY, get_localized_string(LANG_0193), path);
		return;
	}

	float scale = (iscale < 0) ? (1.0f / iscale) : (1.0f * iscale);

	scale = std::fabs(scale);

	std::string file_name = lightmapmode ? bsp_name + "_lightmap" : bsp_name;

	if (export_collision)
		file_name += "_collision";

	print_log(get_localized_string(LANG_0194), file_name + ".obj", path);
	print_log(get_localized_string(LANG_0195), iscale == 1 ? "scale" : iscale < 0 ? "downscale" : "upscale", abs(iscale));

	std::string groupname = std::string();

	BspRenderer* bsprend = renderer;

	remove_faces_by_content(CONTENTS_SKY);
	remove_faces_by_content(CONTENTS_SOLID);

	remove_unused_model_structures();

	int merged = merge_all_verts(0.1f);
	print_log(PRINT_RED, " Merged {} verts \n", merged);
	remove_unused_model_structures(CLEAN_EDGES_FORCE | CLEAN_TEXINFOS_FORCE);


	save_undo_lightmaps();
	resize_all_lightmaps();

	bsprend->reuploadTextures();
	bsprend->loadLightmaps();

	//g_app->reloading = true;
	//bsprend->reload();
	//g_app->reloading = false;

	createDir(path + "textures");
	std::vector<std::string> materials;
	std::vector<std::string> matnames;

	const int null_material_idx = 0;
	matnames.push_back("NULL");
	materials.emplace_back("");
	materials.emplace_back("newmtl NULL");
	materials.emplace_back("Ns 0");
	materials.emplace_back("Ka 1 1 1");
	materials.emplace_back("Ks 0 0 0");
	materials.emplace_back("Ke 0 0 0");
	materials.emplace_back("Ni 1");
	materials.emplace_back("Kd 0.5 0.5 0.5");
	materials.emplace_back("d 1");
	materials.emplace_back("illum 2");

	int vertoffset = 1;
	int normoffset = 0;

	int materialid = -1;
	int lastmaterialid = -2;

	std::set<int> refreshedModels;

	ProgressMeter tmp = g_progress;

	if (lightmapmode) {
		exportLightmaps(path, bsp_name, materials, matnames, bsprend);
	} else {
		exportSkybox(path, bsp_name, materials, matnames, this, bsprend);
	}

	if (with_mdl) {
		processMdlToBsp(this, tmp);
	} else {
		exportSeparateMdls(path, scale, this, tmp);
	}

	exportSeparateSprs(path, scale, this, tmp);

	tmp.update("Export to obj...", faceCount);

	g_progress = tmp;

	std::map<std::string, std::stringstream> group_verts;
	std::map<std::string, std::stringstream> group_normals;
	std::map<std::string, std::stringstream> group_textures;
	std::map<std::string, std::stringstream> group_objects;

	std::map<std::string, int> group_vert_groups;

	std::vector<std::string> group_list;

	for (int i = 0; i < faceCount && !export_collision; i++)
	{
		tmp.tick();
		g_progress = tmp;

		int mdlid = get_model_from_face(i);
		RenderFace* rface;
		RenderGroup* rgroup;

		if (refreshedModels.find(mdlid) == refreshedModels.end())
		{
			bsprend->refreshModel(mdlid, false, false);
			refreshedModels.insert(mdlid);
		}

		if (!bsprend->getRenderPointers(i, &rface, &rgroup))
		{
			print_log(PRINT_RED | PRINT_INTENSITY, get_localized_string(LANG_0196));
			continue;
		}

		BSPFACE32& face = faces[i];
		BSPTEXTUREINFO& texinfo = texinfos[face.iTextureInfo];
		int texOffset = ((int*)textures)[texinfo.iMiptex + 1];
		BSPMIPTEX tex = BSPMIPTEX();

		if (texOffset >= 0)
			tex = *((BSPMIPTEX*)(textures + texOffset));

		std::vector<int> entIds = get_model_ents_ids(mdlid);

		if (entIds.empty())
		{
			entIds.push_back(0);
		}

		materialid = lightmapmode
			? (renderer->lightmaps[i].atlasId[0] + 1)
			: addTextureMaterial(path, materials, matnames, this, tex, texinfo, texOffset);

		if (materialid < 0) materialid = null_material_idx;

		for (size_t e = 0; e < entIds.size(); e++)
		{
			int tmpentid = entIds[e];

			Entity* ent = ents[tmpentid];

			RenderEnt* rendEnt = &renderer->renderEnts[tmpentid];

			vec3 origin_offset = ent->origin.flip();

			std::string next_group_name = "M_" + std::to_string(mdlid) + "_ENT_" + std::to_string(tmpentid) + "#" + ent->classname;
			//print_log("Generating {}\n", next_group_name);

			if (next_group_name != groupname)
			{
				groupname = std::move(next_group_name);

				if (std::find(group_list.begin(), group_list.end(), groupname) == group_list.end())
					group_list.push_back(groupname);
			}

			mat4x4 angle_mat;
			angle_mat.loadIdentity();

			if (rendEnt->needAngles)
			{
				vec3 angles = rendEnt->angles;

				angles.z = -angles.x;
				angles.x = angles.z;

				renderer->setRenderAngles(ent->classname, angle_mat, angles);
			}


			BSPPLANE tmpPlane = getPlaneFromFace(&face);
			vec3 org_norm = tmpPlane.vNormal;

			if (rendEnt->needAngles)
			{
				org_norm = (angle_mat * vec4(org_norm, 1.0)).xyz();
			}

			org_norm = org_norm.flip();

			for (int n = rface->vertCount - 1; n >= 0; n--)
			{
				lightmapVert& vert = ((lightmapVert*)rgroup->buffer->get_data())[rface->vertOffset + n];

				vec3 org_pos = vert.pos;

				if (rendEnt->needAngles)
				{
					org_pos = (angle_mat * vec4(org_pos, 1.0)).xyz();
				}

				org_pos += origin_offset;

				org_pos *= scale;

				group_verts[groupname] << "v " << org_pos.toKeyvalueString() << "\n";
			}

			group_normals[groupname] << "vn " << org_norm.toKeyvalueString() << "\n";

			normoffset++;

			for (int n = rface->vertCount - 1; n >= 0; n--)
			{
				lightmapVert& vert = ((lightmapVert*)rgroup->buffer->get_data())[rface->vertOffset + n];

				vec3 org_pos = vert.pos;

				if (rendEnt->needAngles)
				{
					org_pos = (angle_mat * vec4(org_pos, 1.0)).xyz();
				}

				org_pos = org_pos.flipUV();

				float fU = dotProduct(texinfo.vS, org_pos) + texinfo.shiftS;
				float fV = dotProduct(texinfo.vT, org_pos) + texinfo.shiftT;

				fU /= (float)tex.nWidth;
				fV /= -(float)tex.nHeight;

				// lightmap UVs
				if (lightmapmode) {
					fU = vert.luv[0][0];
					fV = 1-vert.luv[0][1];
				}

				group_textures[groupname] << "vt " << flt_to_str(fU) << " " << flt_to_str(fV) << "\n";
			}

			if (lastmaterialid != materialid)
			{
				group_vert_groups[groupname]++;

				if (grouping == 1)
				{
					group_objects[groupname] << "g " << groupname << "_f" << group_vert_groups[groupname] << "\n";
				}
				else if (grouping == 2)
				{
					group_objects[groupname] << "o " << groupname << "_f" << group_vert_groups[groupname] << "\n";
				}

				if (materialid >= 0)
				{
					group_objects[groupname] << "usemtl " << matnames[materialid] << "\n";
				}
			}

			lastmaterialid = materialid;

			group_objects[groupname] << "f";

			for (int n = 0; n < rface->vertCount; n++)
			{
				int id = vertoffset + n;

				group_objects[groupname] << " " << id << "/" << id << "/" << normoffset;
			}

			group_objects[groupname] << "\n";

			vertoffset += rface->vertCount;
		}
	}

	// Ensure clipnodes are loaded for model clipnode export
	if (!bsprend->clipnodesLoaded)
	{
		bsprend->loadClipnodes();
		bsprend->clipnodesLoaded = true;
	}

	// Export model clipnode geometry for models without faces
	for (int m = 0; m < modelCount; m++)
	{
		if (bsprend->renderModels.empty() || m >= (int)bsprend->renderModels.size() || !bsprend->renderModels[m]->renderGroups.empty())
			continue;

		int hullIdx = bsprend->getBestClipnodeHull(m);
		if (hullIdx != -1)
		{
			exportModelClipnodeGeometry(this, group_list, group_verts, group_normals, group_textures, group_objects, group_vert_groups, matnames, vertoffset, normoffset, scale, grouping, materialid, lastmaterialid, tmp, m, hullIdx, null_material_idx);
		}
	}

	// Export collision geometry
	if (export_collision)
	{
		exportCollisionGeometry(this, group_list, group_verts, group_normals, group_textures, group_objects, group_vert_groups, matnames, vertoffset, normoffset, scale, grouping, materialid, lastmaterialid, tmp, modelCount, null_material_idx);
	}

	std::ofstream obj_file(path + file_name + ".obj", std::ios::binary);
	if (obj_file)
	{
		obj_file << "# Exported using bspguy!\n";
		obj_file << "mtllib " << file_name << ".mtl\n";

		for (auto& group : group_list)
		{
			if (grouping == 0 || grouping == 1)
				obj_file << "o " << group << "\n";
			else if (grouping == 2)
				obj_file << "g " << group << "\n";

			obj_file << group_verts[group].str();

			obj_file << group_normals[group].str();

			obj_file << group_textures[group].str();

			obj_file << group_objects[group].str();
		}

		obj_file.flush();
		obj_file.close();
	}


	std::ofstream mat_file(path + file_name + ".mtl", std::ios::binary);

	if (mat_file)
	{
		mat_file << "# Exported using bspguy!\n";

		for (auto const& s : materials)
		{
			mat_file << s << '\n';
		}

		mat_file.flush();
		mat_file.close();
	}

	renderer->undo();

	for (auto m : refreshedModels)
		bsprend->refreshModel(m, false);
}

static void collect_clipnode_indices(Bsp* bsp, std::set<int>& seen, int nodeidx) {
	if (nodeidx < 0) return;
	if (seen.count(nodeidx)) return;
	seen.insert(nodeidx);
	const BSPCLIPNODE32& cn = bsp->clipnodes[nodeidx];
	collect_clipnode_indices(bsp, seen, cn.iChildren[0]);
	collect_clipnode_indices(bsp, seen, cn.iChildren[1]);
}

static void collect_node_indices(Bsp* bsp, std::set<int>& nodeSet, int nodeidx) {
	if (nodeidx < 0) return;
	if (nodeSet.count(nodeidx)) return;
	nodeSet.insert(nodeidx);
	const BSPNODE32& node = bsp->nodes[nodeidx];
	collect_node_indices(bsp, nodeSet, node.iChildren[0]);
	collect_node_indices(bsp, nodeSet, node.iChildren[1]);
}

static void collect_leaf_indices_from_nodes(Bsp* bsp, std::set<int>& leafSet, int nodeidx) {
	if (nodeidx < 0) {
		leafSet.insert(-(nodeidx + 1));
		return;
	}
	const BSPNODE32& node = bsp->nodes[nodeidx];
	collect_leaf_indices_from_nodes(bsp, leafSet, node.iChildren[0]);
	collect_leaf_indices_from_nodes(bsp, leafSet, node.iChildren[1]);
}

static void collect_nodes_from_leaves(Bsp* bsp, const std::set<int>& leafSet, std::set<int>& nodeSet, int nodeidx) {
	if (nodeidx < 0) return;

	bool hasDescendantLeaf = false;

	const BSPNODE32& node = bsp->nodes[nodeidx];

	// Check left child
	if (node.iChildren[0] >= 0) {
		collect_nodes_from_leaves(bsp, leafSet, nodeSet, node.iChildren[0]);
	} else {
		int leafidx = -(node.iChildren[0] + 1);
		if (leafSet.count(leafidx)) hasDescendantLeaf = true;
	}

	// Check right child
	if (node.iChildren[1] >= 0) {
		collect_nodes_from_leaves(bsp, leafSet, nodeSet, node.iChildren[1]);
	} else {
		int leafidx = -(node.iChildren[1] + 1);
		if (leafSet.count(leafidx)) hasDescendantLeaf = true;
	}

	// If this node has any descendant leaf in the set, include it
	if (hasDescendantLeaf) {
		nodeSet.insert(nodeidx);
	} else {
		// Check if this node directly points to a leaf in the set (though unlikely in standard BSP)
		// But already checked above
	}
}

void Bsp::ExportBspJson(const std::string& path)
{
	std::stringstream json;
	json << "{\n";

	json << "    \"nodes\": {\n";

	for (int i = 0; i < this->nodeCount; i++)
	{
		const BSPNODE32& node = this->nodes[i];
		if (i > 0) json << ",\n";
		json << "        \"" << i << "\": {\n";
		json << "            \"node_index\": " << i << ",\n";
		const BSPPLANE& plane = this->planes[node.iPlane];
		json << "            \"plane\": {\"normal\":[" << plane.vNormal.x << "," << plane.vNormal.y << "," << plane.vNormal.z << "],\"dist\":" << plane.fDist << "},\n";
		json << "            \"children\": [" << node.iChildren[0] << "," << node.iChildren[1] << "],\n";
		json << "            \"mins\": [" << node.nMins.x << "," << node.nMins.y << "," << node.nMins.z << "],\n";
		json << "            \"maxs\": [" << node.nMaxs.x << "," << node.nMaxs.y << "," << node.nMaxs.z << "]\n";
		json << "        }";
	}

	json << "\n    },\n";

	json << "    \"clipnodes\": {\n";

	for (int i = 0; i < this->clipnodeCount; i++)
	{
		const BSPCLIPNODE32& cn = this->clipnodes[i];
		if (i > 0) json << ",\n";
		json << "        \"" << i << "\": {\n";
		json << "            \"node_index\": " << i << ",\n";
		const BSPPLANE& plane = this->planes[cn.iPlane];
		json << "            \"plane\": {\"normal\":[" << plane.vNormal.x << "," << plane.vNormal.y << "," << plane.vNormal.z << "],\"dist\":" << plane.fDist << "},\n";
		json << "            \"children\": [" << cn.iChildren[0] << "," << cn.iChildren[1] << "]\n";
		json << "        }";
	}
	json << "\n    },\n";

	std::vector<int> leaf_models(leafCount, -1);
	for (int i = 0; i < this->leafCount; i++)
	{
		BSPLEAF32& leaf = this->leaves[i];
		int model_index = -1;
		if (leaf.nMarkSurfaces > 0) {
			int first_model = -1;
			bool all_same = true;
			for (int m = 0; m < leaf.nMarkSurfaces; m++) {
				int faceIdx = marksurfs[leaf.iFirstMarkSurface + m];
				int face_model = this->get_model_from_face(faceIdx);
				if (first_model == -1) first_model = face_model;
				else if (first_model != face_model) {
				   all_same = false;
				   break;
				}
			}
			if (all_same) model_index = first_model;
		}
		leaf_models[i] = model_index;
	}

	json << "    \"leaves\": {\n";
	int leafCount = 0;
	for (int i = 0; i < this->leafCount; i++)
	{
		BSPLEAF32& leaf = this->leaves[i];
		if (leafCount > 0) json << ",\n";
		json << "        \"" << i << "\": {\n";
		json << "            \"leaf_index\": " << i << ",\n";
		json << "            \"contents\": " << leaf.nContents << ",\n";
		json << "            \"mins\": [" << leaf.nMins.x << ", " << leaf.nMins.y << ", " << leaf.nMins.z << "],\n";
		json << "            \"maxs\": [" << leaf.nMaxs.x << ", " << leaf.nMaxs.y << ", " << leaf.nMaxs.z << "],\n";

		/*
		std::vector<int> planeIndices;
		for (int m = 0; m < leaf.nMarkSurfaces; m++)
		{
			int faceIdx = marksurfs[leaf.iFirstMarkSurface + m];
			planeIndices.push_back(faces[faceIdx].iPlane);
		}

		json << "            \"planes\": [";
		int planeCount = 0;
		for (int planeIdx : planeIndices)
		{
			const BSPPLANE& plane = planes[planeIdx];
			if (planeCount > 0) json << ",";
			json << "\n                {\"normal\":[" << plane.vNormal.x << "," << plane.vNormal.y << "," << plane.vNormal.z << "],\"dist\":" << plane.fDist << "}";
			planeCount++;
		}
		json << "\n            ],\n";
		*/

		json << "            \"model_index\": " << leaf_models[i] << "\n";
		json << "        }";
		leafCount++;
	}

	json << "\n    },\n";

	json << "    \"models\": {\n";

	int modelCountJson = 0;
	for (int i = 0; i < this->modelCount; i++)
	{
		BSPMODEL& model = this->models[i];
		if (modelCountJson > 0) json << ",\n";
		json << "        \"" << i << "\": {\n";
		json << "            \"model_index\": " << i << ",\n";
		json << "            \"mins\": [" << model.nMins.x << ", " << model.nMins.y << ", " << model.nMins.z << "],\n";
		json << "            \"maxs\": [" << model.nMaxs.x << ", " << model.nMaxs.y << ", " << model.nMaxs.z << "],\n";
		json << "            \"face_count\": " << model.nFaces << ",\n";

		/*
		std::vector<int> planeIndices;
		std::map<int, int> planeSide;
		for (int f = model.iFirstFace; f < model.iFirstFace + model.nFaces; f++)
		{
			int planeIdx = faces[f].iPlane;
			planeIndices.push_back(planeIdx);
			planeSide[planeIdx] = faces[f].nPlaneSide; // from face, not texture
		}

		json << "            \"planes\": [";
		int planeCount = 0;
		for (int planeIdx : planeIndices)
		{
			const BSPPLANE& plane = planes[planeIdx];
			vec3 normal = plane.vNormal;
			float dist = plane.fDist;
			if (planeSide[planeIdx] == 1) // back side, flip
			{
				normal = -normal;
				dist = -dist;
			}
			if (planeCount > 0) json << ",";
			json << "\n                {\"normal\":[" << normal.x << "," << normal.y << "," << normal.z << "],\"dist\":" << dist << "}";
			planeCount++;
		}
		json << "\n            ],\n";
		*/

		std::vector<int> entIds = this->get_model_ents_ids(i);
		int entIdx = entIds.empty() ? -1 : entIds[0];
		std::string classname = (entIdx >= 0 && entIdx < (int)this->ents.size()) ? json_escape(this->ents[entIdx]->classname) : "worldspawn";

		json << "            \"entity_info\": {\n";
		json << "                \"entity_index\": " << entIdx << ",\n";
		json << "                \"classname\": \"" << classname << "\"\n";
		json << "            },\n";

		/*
		std::vector<int> leaf_indices;
		for (int l = 0; l < this->leafCount; l++) {
			if (leaf_models[l] == i) leaf_indices.push_back(l);
		}

		json << "            \"leaf_indices\": [";
		for (size_t k = 0; k < leaf_indices.size(); k++) {
			if (k > 0) json << ",";
			json << leaf_indices[k];
		}
		json << "],\n";
		*/

		json << "            \"hulls\": [";
		int hullCountJson = 0;
		for (int h = 0; h < 4; h++) {
			if (model.iHeadnodes[h] < 0) continue;
			if (hullCountJson++ > 0) json << ",";
			json << "\n                {\n";
			json << "                    \"hull_index\": " << h << ",\n";
			json << "                    \"headnode\": " << model.iHeadnodes[h] << ",\n";
			json << "                    \"clipnodes\": {";
			std::set<int> clipnodeSet;
			collect_clipnode_indices(this, clipnodeSet, model.iHeadnodes[h]);
			int clipCount = 0;
			for (auto cnidx : clipnodeSet) {
				const BSPCLIPNODE32& cn = clipnodes[cnidx];
				const BSPPLANE& plane = planes[cn.iPlane];
				if (clipCount++ > 0) json << ",";
				json << "\n                        \"" << cnidx << "\": {\"node_index\":" << cnidx << ", \"plane\":{\"normal\":[" << plane.vNormal.x << "," << plane.vNormal.y << "," << plane.vNormal.z << "],\"dist\":" << plane.fDist << "}, \"children\":[" << cn.iChildren[0] << "," << cn.iChildren[1] << "]}";
			}
			json << "\n                    }\n";
			json << "                }";
		}
		json << "\n            ]\n";

		/*
		json << "            \"geometry_nodes\": [";
		std::set<int> nodeSet;
		collect_nodes_from_leaves(this, leaf_indices.empty() ? std::set<int>() : std::set<int>(leaf_indices.begin(), leaf_indices.end()), nodeSet, 0);
		for (auto nit = nodeSet.begin(); nit != nodeSet.end(); ++nit) {
			if (nit != nodeSet.begin()) json << ",";
			json << *nit;
		}
		json << "],\n";
		*/

		/*
		json << "            \"geometry_leaves\": [";
		for (size_t k = 0; k < leaf_indices.size(); k++) {
			if (k > 0) json << ",";
			json << leaf_indices[k];
		}
		json << "]\n";
		*/


		json << "        }";
		modelCountJson++;
	}

	json << "\n    },\n";

	json << "    \"entities\": {\n";

	int entityCount = 0;
	for (size_t i = 0; i < this->ents.size(); i++)
	{
		Entity* ent = this->ents[i];
		if (entityCount > 0) json << ",\n";
		json << "        \"" << i << "\": {\n";
		json << "            \"entity_index\": " << i << ",\n";
		json << "            \"classname\": \"" << json_escape(ent->classname) << "\",\n";
		json << "            \"keyvalues\": {\n";

		int kvCount = 0;
		for (auto& kv : ent->keyvalues)
		{
			if (kvCount > 0) json << ",\n";
			json << "                \"" << json_escape(kv.first) << "\": \"" << json_escape(kv.second) << "\"";
			kvCount++;
		}

		json << "\n            }\n";
		json << "        }";
		entityCount++;
	}

	json << "\n    }\n";
	json << "}\n";

	std::string filename = path + "/bsp.json";
	createDir(path);

	std::ofstream file(filename);
	if (file.is_open())
	{
		file << json.str();
		file.close();
		print_log("Exported bsp.json to {}\n", filename);
	}
	else
	{
		print_log(PRINT_RED, "Failed to open file for writing: {}\n", filename);
	}
}
