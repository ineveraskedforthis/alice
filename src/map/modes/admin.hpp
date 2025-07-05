#pragma once

std::vector<uint32_t> admin_map_from(sys::state& state) {
	uint32_t province_size = state.world.province_size();
	uint32_t texture_size = province_size + 256 - province_size % 256;

	std::vector<uint32_t> prov_color(texture_size * 2);
	dcon::province_id selected_province = state.map_state.get_selected_province();
	dcon::nation_id selected_nation = selected_province
		? state.world.province_get_nation_from_province_ownership(selected_province)
		: dcon::nation_id{};
	state.world.for_each_province([&](dcon::province_id prov_id) {
		auto i = province::to_map_id(prov_id);
		auto fat_id = dcon::fatten(state.world, prov_id);
		auto owner = fat_id.get_nation_from_province_ownership();
		auto sphere_leader = owner.get_in_sphere_of();
		auto overlord = owner.get_overlord_as_subject().get_ruler();
		auto selected_is_subject = false;
		for(auto subject : owner.get_overlord_as_ruler()) {
			if(subject.get_subject() == selected_nation)
				selected_is_subject = true;
		}
		if(
			!selected_nation
			|| owner == selected_nation
			|| sphere_leader == selected_nation
			|| overlord == selected_nation
			|| selected_is_subject
		) {
			auto admin_efficiency = fat_id.get_control_ratio();
			uint32_t color = ogl::color_gradient_viridis(admin_efficiency);
			prov_color[i] = color;
			prov_color[i + texture_size] = color;
		} else {
			prov_color[i] = 0;
			prov_color[i + texture_size] = 0;
		}
	});
	return prov_color;
}
