# every frame is a function of its number alone, so two runs of the scene, on two rendering drivers, are the same
# frames. the scene goes through phases of PHASE frames, each adding to what the renderer has to do
extends Node3D

const PHASE := 24
var camera := Camera3D.new()
var environment := Environment.new()
var frame := 0
var spinning: Array[Node3D] = []


func material(color: Color, metallic := 0.0, roughness := 0.5) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	m.metallic = metallic
	m.roughness = roughness
	return m


func textured(seed_: int) -> StandardMaterial3D:
	# noise made on the CPU from a seed: albedo with mipmaps, and a normal map from the same noise
	var noise := FastNoiseLite.new()
	noise.seed = seed_
	noise.frequency = 0.05
	var albedo := NoiseTexture2D.new()
	albedo.noise = noise
	albedo.width = 256
	albedo.height = 256
	albedo.generate_mipmaps = true
	albedo.color_ramp = Gradient.new()
	albedo.color_ramp.colors = PackedColorArray([Color(0.8, 0.2, 0.1), Color(0.1, 0.3, 0.9)])
	var normal := NoiseTexture2D.new()
	normal.noise = noise
	normal.width = 256
	normal.height = 256
	normal.as_normal_map = true
	var m := StandardMaterial3D.new()
	m.albedo_texture = albedo
	m.normal_enabled = true
	m.normal_texture = normal
	m.uv1_scale = Vector3(3, 3, 3)
	m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	return m


func mesh(shape: Mesh, at: Vector3, m: Material, spins := false) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.mesh = shape
	instance.position = at
	instance.material_override = m
	add_child(instance)
	if spins:
		spinning.append(instance)
	return instance


func _ready() -> void:
	camera.fov = 60
	add_child(camera)
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 30, 0)
	sun.shadow_enabled = true
	add_child(sun)

	var floor_ := PlaneMesh.new()
	floor_.size = Vector2(40, 40)
	mesh(floor_, Vector3.ZERO, textured(1))
	mesh(BoxMesh.new(), Vector3(-3, 0.5, 0), textured(2), true)
	mesh(SphereMesh.new(), Vector3(0, 0.5, 0), material(Color(0.9, 0.9, 0.9), 1.0, 0.1))
	mesh(TorusMesh.new(), Vector3(3, 0.7, 0), material(Color(0.9, 0.7, 0.1), 0.6, 0.3), true)
	mesh(CylinderMesh.new(), Vector3(0, 1, -4), material(Color(0.2, 0.8, 0.3)))
	mesh(CapsuleMesh.new(), Vector3(-5, 1, -3), material(Color(0.7, 0.2, 0.7), 0.0, 0.9), true)


# what a phase adds, once, at its first frame
func enter(phase: int) -> void:
	match phase:
		1:
			# point and spot lights with shadows of their own
			for i in 4:
				var lamp := OmniLight3D.new()
				lamp.position = Vector3(-6 + 4 * i, 2.5, 2)
				lamp.light_color = Color.from_hsv(i / 4.0, 0.8, 1)
				lamp.omni_range = 8
				lamp.shadow_enabled = true
				add_child(lamp)
			var spot := SpotLight3D.new()
			spot.position = Vector3(0, 6, 4)
			spot.rotation_degrees = Vector3(-60, 0, 0)
			spot.spot_range = 15
			spot.shadow_enabled = true
			add_child(spot)
		2:
			# many instances in one draw, glass, and a decal
			var many := MultiMesh.new()
			many.transform_format = MultiMesh.TRANSFORM_3D
			many.use_colors = true
			many.mesh = BoxMesh.new()
			many.instance_count = 2000
			for i in many.instance_count:
				var at := Vector3(i % 50 - 25, 0.15, -6 - i / 50) * 0.6
				many.set_instance_transform(i, Transform3D(Basis().scaled(Vector3.ONE * 0.3), at))
				many.set_instance_color(i, Color.from_hsv(i / 2000.0, 0.7, 0.9))
			var instances := MultiMeshInstance3D.new()
			instances.multimesh = many
			var colored := material(Color.WHITE)
			colored.vertex_color_use_as_albedo = true
			instances.material_override = colored
			add_child(instances)
			var glass := material(Color(0.6, 0.9, 1.0, 0.35), 0.0, 0.05)
			glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
			glass.refraction_enabled = true
			mesh(SphereMesh.new(), Vector3(1.5, 1.2, 2), glass, true)
			var decal := Decal.new()
			decal.position = Vector3(-1.5, 0, 2)
			decal.size = Vector3(3, 2, 3)
			decal.texture_albedo = (textured(3).albedo_texture)
			add_child(decal)
		3:
			# particles simulated on the GPU, from a seed
			var process := ParticleProcessMaterial.new()
			process.direction = Vector3.UP
			process.initial_velocity_min = 3
			process.initial_velocity_max = 5
			process.spread = 25
			var particles := GPUParticles3D.new()
			particles.amount = 4000
			particles.lifetime = 2
			particles.use_fixed_seed = true
			particles.seed = 7
			particles.process_material = process
			var quad := QuadMesh.new()
			quad.size = Vector2(0.08, 0.08)
			var glow := material(Color(1, 0.6, 0.2))
			glow.emission_enabled = true
			glow.emission = Color(1, 0.5, 0.1)
			glow.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
			quad.material = glow
			particles.draw_pass_1 = quad
			particles.position = Vector3(5, 0, 2)
			add_child(particles)
		4:
			# the screen-space passes
			environment.ssao_enabled = true
			environment.ssil_enabled = true
			environment.ssr_enabled = true
			environment.glow_enabled = true
		5:
			# froxel fog and signed distance field global illumination: volumes written by compute
			environment.volumetric_fog_enabled = true
			environment.volumetric_fog_density = 0.03
			environment.sdfgi_enabled = true
		6:
			# multisampling, then a temporal upscaler from half resolution
			get_viewport().msaa_3d = Viewport.MSAA_4X
		7:
			get_viewport().msaa_3d = Viewport.MSAA_DISABLED
			get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
			get_viewport().scaling_3d_scale = 0.5
		8:
			# a second view drawn into a texture that the scene then shows, and 2D drawn over all
			get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			get_viewport().scaling_3d_scale = 1.0
			var inner := SubViewport.new()
			inner.size = Vector2i(256, 256)
			inner.render_target_update_mode = SubViewport.UPDATE_ALWAYS
			var eye := Camera3D.new()
			eye.position = Vector3(0, 12, 0.01)
			eye.rotation_degrees = Vector3(-90, 0, 0)
			inner.add_child(eye)
			add_child(inner)
			var screen := material(Color.WHITE)
			screen.albedo_texture = inner.get_texture()
			screen.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
			var panel := QuadMesh.new()
			panel.size = Vector2(3, 3)
			mesh(panel, Vector3(-4, 2.5, -1), screen)
			var layer := CanvasLayer.new()
			var label := Label.new()
			label.text = "mullion-test 0123456789"
			label.position = Vector2(16, 16)
			layer.add_child(label)
			var bar := ColorRect.new()
			bar.color = Color(0.1, 0.9, 0.4, 0.6)
			bar.position = Vector2(16, 48)
			bar.size = Vector2(200, 12)
			layer.add_child(bar)
			add_child(layer)


func _process(_delta: float) -> void:
	if frame % PHASE == 0:
		enter(frame / PHASE)
	var turn := frame * TAU / 240
	camera.position = Vector3(9 * sin(turn), 4 + sin(turn * 2), 9 * cos(turn))
	camera.look_at(Vector3(0, 0.5, 0))
	for node in spinning:
		node.rotation = Vector3(turn * 2, turn * 3, 0)
	frame += 1
