struct UserCmd
{
	uint32_t SequenceNumber; // #1, #2, #3...
	float ForwardMove;
	float SideMove;
	float ViewYaw;
	float ViewPitch;
	uint32_t Buttons;    // Bitmask: JUMP | FIRE | CROUCH
	uint32_t DurationMS; // How long this command lasted
};