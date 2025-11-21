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

static void exportLightmaps(const std::string& path, std::vector<std::string>& materials, std::vector<std::string>& matnames, BspRenderer* bsprend)
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
			"atlases", "atlas_" + std::to_string(i),
			rgba_data, atlas_tex->width, atlas_tex->height
		);

		delete rgba_data;
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

static void exportMdlToObj(const std::string& output_path, StudioModel* mdl, const std::string& name, float scale, ProgressMeter& tmp)
{
	mdl->UpdateModelMeshList();

	std::string path = output_path + "/mdl_models/";
	createDir(path);
	createDir(path + "textures");

	std::string obj_name = name + ".obj";
	std::string mtl_name = name + ".mtl";

	std::vector<std::string> materials;
	std::vector<std::string> matnames;

	int vertoffset = 1;
	int texoffset = 1;

	std::ofstream obj_file(path + obj_name);
	if (!obj_file) return;

	obj_file << "# Exported MDL using bspguy!\n";
	obj_file << "mtllib " << name << ".mtl\n";

	std::ofstream mtl_file(path + mtl_name);
	if (mtl_file) {
		mtl_file << "# Exported MDL mtl using bspguy!\n";
	}

	std::map<Texture*, int> texToMatId;
	int lastMaterial = -1;

	for (size_t group = 0; group < mdl->mdl_mesh_groups.size(); group++) {
		for (size_t meshid = 0; meshid < mdl->mdl_mesh_groups[group].size(); meshid++) {
			tmp.tick();
			StudioMesh& sm = mdl->mdl_mesh_groups[group][meshid];

			// Ensure material for this texture
			int currentMaterial = -1;
			if (sm.texture) {
				std::string texname = sm.texture->texName.empty() ? "unnamed" : sm.texture->texName;
				if (texToMatId.find(sm.texture) == texToMatId.end()) {
					currentMaterial = addMdlTextureMaterial(path, materials, matnames, sm.texture, texname);
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
				for (size_t j = 0; j < 3; j++) {
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
	tmp.tick();
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

	if (lightmapmode)
		exportLightmaps(path, materials, matnames, bsprend);

	if (with_mdl)
		processMdlToBsp(this, tmp);
	else {
		int mdl_count = 0;
		for (size_t ent = 0; ent < ents.size(); ent++)
		{
			if (renderer->renderEnts[ent].mdl)
			{
				mdl_count++;
			}
		}
		if (mdl_count > 0) {
			tmp.update("EXPORT MDL...", mdl_count);
			g_progress = tmp;
			for (size_t ent = 0; ent < ents.size(); ent++)
			{
				if (renderer->renderEnts[ent].mdl)
				{
					StudioModel* mdl = (StudioModel*)renderer->renderEnts[ent].mdl;
					std::string model_path = ents[ent]->keyvalues["model"];
					fs::path p(model_path);
					std::string model_name = p.empty() ? "unknown" : stripExt(p.filename().string());
					std::string name = model_name + "_" + std::to_string(ent);
					exportMdlToObj(path, mdl, name, scale, tmp);
				}
			}
		} else {
			renderer->preRenderEnts();
		}
	}

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
			print_log("Generating {}\n", next_group_name);

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
