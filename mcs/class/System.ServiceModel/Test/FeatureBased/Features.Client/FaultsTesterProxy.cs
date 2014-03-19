﻿//------------------------------------------------------------------------------
// <auto-generated>
//     This code was generated by a tool.
//     Runtime Version:2.0.50727.1433
//
//     Changes to this file may cause incorrect behavior and will be lost if
//     the code is regenerated.
// </auto-generated>
//------------------------------------------------------------------------------

namespace Proxy.MonoTests.Features.Client
{
	[System.CodeDom.Compiler.GeneratedCodeAttribute ("System.ServiceModel", "3.0.0.0")]
	[System.ServiceModel.ServiceContractAttribute (Namespace = "http://MonoTests.Features.Contracts", ConfigurationName = "IFaultsTesterContract")]
	public interface IFaultsTesterContract
	{
		[System.ServiceModel.OperationContractAttribute (Action = "http://MonoTests.Features.Contracts/IFaultsTesterContract/FaultMethod")]
		void FaultMethod (string faultText);
	}

	[System.CodeDom.Compiler.GeneratedCodeAttribute ("System.ServiceModel", "3.0.0.0")]
	public interface IFaultsTesterContractChannel : IFaultsTesterContract, System.ServiceModel.IClientChannel
	{
	}

	[System.Diagnostics.DebuggerStepThroughAttribute ()]
	[System.CodeDom.Compiler.GeneratedCodeAttribute ("System.ServiceModel", "3.0.0.0")]
	public partial class FaultsTesterContractClient : System.ServiceModel.ClientBase<IFaultsTesterContract>, IFaultsTesterContract
	{

		public FaultsTesterContractClient ()
		{
		}

		public FaultsTesterContractClient (string endpointConfigurationName) :
			base (endpointConfigurationName)
		{
		}

		public FaultsTesterContractClient (string endpointConfigurationName, string remoteAddress) :
			base (endpointConfigurationName, remoteAddress)
		{
		}

		public FaultsTesterContractClient (string endpointConfigurationName, System.ServiceModel.EndpointAddress remoteAddress) :
			base (endpointConfigurationName, remoteAddress)
		{
		}

		public FaultsTesterContractClient (System.ServiceModel.Channels.Binding binding, System.ServiceModel.EndpointAddress remoteAddress) :
			base (binding, remoteAddress)
		{
		}

		public void FaultMethod (string faultText)
		{
			base.Channel.FaultMethod (faultText);
		}
	}

	//====================


	[System.CodeDom.Compiler.GeneratedCodeAttribute ("System.ServiceModel", "3.0.0.0")]
	[System.ServiceModel.ServiceContractAttribute (Namespace = "http://MonoTests.Features.Contracts", ConfigurationName = "IFaultsTesterContractIncludeDetails")]
	public interface IFaultsTesterContractIncludeDetails
	{
		[System.ServiceModel.OperationContractAttribute (Action = "http://MonoTests.Features.Contracts/IFaultsTesterContractIncludeDetails/FaultMethod")]
		void FaultMethod (string faultText);
	}

	[System.CodeDom.Compiler.GeneratedCodeAttribute ("System.ServiceModel", "3.0.0.0")]
	public interface IFaultsTesterContractChannelIncludeDetails : IFaultsTesterContractIncludeDetails, System.ServiceModel.IClientChannel
	{
	}

	[System.Diagnostics.DebuggerStepThroughAttribute ()]
	[System.CodeDom.Compiler.GeneratedCodeAttribute ("System.ServiceModel", "3.0.0.0")]
	public partial class FaultsTesterContractClientIncludeDetails : System.ServiceModel.ClientBase<IFaultsTesterContractIncludeDetails>, IFaultsTesterContractIncludeDetails
	{

		public FaultsTesterContractClientIncludeDetails ()
		{
		}

		public FaultsTesterContractClientIncludeDetails (string endpointConfigurationName) :
			base (endpointConfigurationName)
		{
		}

		public FaultsTesterContractClientIncludeDetails (string endpointConfigurationName, string remoteAddress) :
			base (endpointConfigurationName, remoteAddress)
		{
		}

		public FaultsTesterContractClientIncludeDetails (string endpointConfigurationName, System.ServiceModel.EndpointAddress remoteAddress) :
			base (endpointConfigurationName, remoteAddress)
		{
		}

		public FaultsTesterContractClientIncludeDetails (System.ServiceModel.Channels.Binding binding, System.ServiceModel.EndpointAddress remoteAddress) :
			base (binding, remoteAddress)
		{
		}

		public void FaultMethod (string faultText)
		{
			base.Channel.FaultMethod (faultText);
		}
	}
}
