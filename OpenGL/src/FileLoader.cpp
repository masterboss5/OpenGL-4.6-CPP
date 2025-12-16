#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include "tiny_obj_loader.h"
#include "StaticGeometry.h"
#include <glm.hpp>

namespace FileLoader {
	static std::string readFile(const std::string& path) {
		std::ifstream file(path);
		std::stringstream buffer;
		buffer << file.rdbuf();

		return buffer.str();
	}

    static StaticGeometry* readObj(const std::string& objPath, const std::string& dirPath) {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warnStream;
        std::string errStream;

        bool success = tinyobj::LoadObj(&attrib, &shapes, &materials, &warnStream, &errStream, objPath.c_str(), dirPath.c_str());
        if (!success) {
            throw std::runtime_error("Failed to load OBJ: " + errStream);
        }

        std::cout << warnStream << std::endl;

        struct Vertex {
            glm::vec3 position;
            glm::vec3 normal;
            glm::vec2 uv;
        };

        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        std::map<std::tuple<int, int, int>, unsigned int> vertexMap;

        for (const auto& shape : shapes) {
            for (const auto& idx : shape.mesh.indices) {
                auto key = std::make_tuple(idx.vertex_index, idx.texcoord_index, idx.normal_index);

                if (vertexMap.count(key) == 0) {
                    Vertex v{};

                    if (idx.vertex_index >= 0) {
                        v.position = {
                            attrib.vertices[3 * idx.vertex_index + 0],
                            attrib.vertices[3 * idx.vertex_index + 1],
                            attrib.vertices[3 * idx.vertex_index + 2]
                        };
                    }

                    if (idx.normal_index >= 0 && attrib.normals.size() >= 3 * (idx.normal_index + 1)) {
                        v.normal = {
                            attrib.normals[3 * idx.normal_index + 0],
                            attrib.normals[3 * idx.normal_index + 1],
                            attrib.normals[3 * idx.normal_index + 2]
                        };
                    }
                    else {
                        v.normal = glm::vec3(0.0f); // fallback
                    }

                    if (idx.texcoord_index >= 0 && attrib.texcoords.size() >= 2 * (idx.texcoord_index + 1)) {
                        v.uv = {
                            attrib.texcoords[2 * idx.texcoord_index + 0],
                            attrib.texcoords[2 * idx.texcoord_index + 1]
                        };
                    }
                    else {
                        v.uv = glm::vec2(0.0f); // fallback
                    }

                    vertexMap[key] = static_cast<unsigned int>(vertices.size());
                    vertices.push_back(v);
                }

                indices.push_back(vertexMap[key]);
            }
        }

        std::vector<float> flatPositions;
        std::vector<float> flatNormals;
        std::vector<float> flatUVs;

        for (const auto& v : vertices) {
            flatPositions.insert(flatPositions.end(), { v.position.x, v.position.y, v.position.z });
            flatNormals.insert(flatNormals.end(), { v.normal.x, v.normal.y, v.normal.z });
            flatUVs.insert(flatUVs.end(), { v.uv.x, v.uv.y });
        }

		Material material(
            Texture("objects/" + materials[0].diffuse_texname),
            Texture("objects/" + materials[0].specular_texname),
            Texture("objects/" + materials[0].normal_texname),
            Texture("objects/" + materials[0].bump_texname),
            Texture("objects/" + materials[0].ambient_texname),
            Texture("objects/" + materials[0].roughness_texname),
            Texture("objects/" + materials[0].emissive_texname),
            32.0f
        );

		std::cout << material.diffuseTexture.isLoaded() << std::endl;
		std::cout << material.specularTexture.isLoaded() << std::endl;
		std::cout << material.normalTexture.isLoaded() << std::endl;
		std::cout << material.heightTexture.isLoaded() << std::endl;
		std::cout << material.ambientOcclusionTexture.isLoaded() << std::endl;
		std::cout << material.roughnessTexture.isLoaded() << std::endl;
		std::cout << material.emissiveTexture.isLoaded() << std::endl;

        return new StaticGeometry(
            "test-01",
            material,
            flatPositions,
            indices,
            flatUVs,
            flatNormals
        );
    }
}